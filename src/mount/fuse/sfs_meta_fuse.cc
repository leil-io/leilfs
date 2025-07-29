/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2019 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"

#include <errno.h>
#include <fuse_lowlevel.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <span>

#include "common/datapack.h"
#include "common/massert.h"
#include "common/special_inode_defs.h"
#include "common/type_defs.h"
#include "mount/exports.h"
#include "mount/fuse/sfs_fuselib/metadata.h"
#include "mount/fuse/sfs_meta_fuse.h"
#include "mount/mastercomm.h"
#include "mount/masterproxy.h"
#include "protocol/SFSCommunication.h"
#include "slogger/slogger.h"

static_assert(FUSE_ROOT_ID == SPECIAL_INODE_ROOT, "invalid value of FUSE_ROOT_ID");

// Number of bytes used for metadata fields (inode, type, etc.) after the name in a directory
// entry
constexpr size_t kDirEntryMetaSize = 5;

// Total stride (name length + metadata) to advance to the next directory entry
constexpr size_t kDirEntryStride = 6;

// Number of bytes reserved for extra fields in a directory entry,
// such as inode, type, and any additional metadata.
constexpr size_t kDirEntryExtraFieldsSize = 9;

struct DirectoryBuffer {
	bool wasRead = false;
	std::vector<uint8_t> buffer;
	size_t size;
	std::mutex lock;
};

struct PathBuffer {
	bool changed;
	std::string path;
	size_t size;
	std::mutex lock;
};

#define IS_SPECIAL_INODE(inode) ((inode)>=SPECIAL_INODE_BASE || (inode)==SPECIAL_INODE_ROOT)

static double entry_cache_timeout = 0.0;
static double attr_cache_timeout = 1.0;

static void sfs_attr_to_stat(inode_t inode, const Attributes &attr, struct stat *stbuf) {
	uint16_t attrmode;
	uint8_t attrtype;
	uint32_t attruid,attrgid,attratime,attrmtime,attrctime,attrnlink;
	uint64_t attrlength;
	const uint8_t *ptr;
	ptr = attr.data();
	attrtype = get8bit(&ptr);
	attrmode = get16bit(&ptr);
	get32bit(&ptr, attruid);
	get32bit(&ptr, attrgid);
	get32bit(&ptr, attratime);
	get32bit(&ptr, attrmtime);
	get32bit(&ptr, attrctime);
	get32bit(&ptr, attrnlink);
	attrlength = get64bit(&ptr);
	stbuf->st_ino = inode;
	if (attrtype==TYPE_FILE || attrtype==TYPE_TRASH || attrtype==TYPE_RESERVED) {
		stbuf->st_mode = S_IFREG | (attrmode & 07777);
	} else {
		stbuf->st_mode = 0;
	}
	stbuf->st_size = attrlength;
	stbuf->st_blocks = (attrlength+511)/512;
	stbuf->st_uid = attruid;
	stbuf->st_gid = attrgid;
	stbuf->st_atime = attratime;
	stbuf->st_mtime = attrmtime;
	stbuf->st_ctime = attrctime;
	stbuf->st_nlink = attrnlink;
}

void sfs_meta_statfs(fuse_req_t req, fuse_ino_t ino) {
	uint64_t totalspace,availspace,trashspace,reservedspace;
	inode_t inodes;
	struct statvfs stfsbuf;
	memset(&stfsbuf,0,sizeof(stfsbuf));

	(void)ino;
	fs_statfs(&totalspace,&availspace,&trashspace,&reservedspace,&inodes);

	stfsbuf.f_namemax = NAME_MAX;
	stfsbuf.f_frsize = 512;
	stfsbuf.f_bsize = 512;
	stfsbuf.f_blocks = trashspace/512+reservedspace/512;
	stfsbuf.f_bfree = reservedspace/512;
	stfsbuf.f_bavail = reservedspace/512;
	stfsbuf.f_files = 1000000000+PKGVERSION;
	stfsbuf.f_ffree = 1000000000+PKGVERSION;
	stfsbuf.f_favail = 1000000000+PKGVERSION;

	fuse_reply_statfs(req,&stfsbuf);
}

void sfs_meta_lookup(fuse_req_t req, fuse_ino_t parent, const char *name) {
	struct fuse_entry_param e;
	inode_t inode;
	memset(&e, 0, sizeof(e));
	inode = 0;
	switch (parent) {
	case SPECIAL_INODE_ROOT:
		if (strcmp(name,".")==0 || strcmp(name,"..")==0) {
			inode = SPECIAL_INODE_ROOT;
		} else if (strcmp(name,SPECIAL_FILE_NAME_META_TRASH)==0) {
			inode = SPECIAL_INODE_META_TRASH;
		} else if (strcmp(name,SPECIAL_FILE_NAME_META_RESERVED)==0) {
			inode = SPECIAL_INODE_META_RESERVED;
		} else if (strcmp(name,SPECIAL_FILE_NAME_MASTERINFO)==0) {
			memset(&e, 0, sizeof(e));
			e.ino = SPECIAL_INODE_MASTERINFO;
			e.attr_timeout = 3600.0;
			e.entry_timeout = 3600.0;
			e.attr = getMasterInfoStat();
			fuse_reply_entry(req, &e);
			return ;
		}
		break;
	case SPECIAL_INODE_META_TRASH:
		if (strcmp(name,".")==0) {
			inode = SPECIAL_INODE_META_TRASH;
		} else if (strcmp(name,"..")==0) {
			inode = SPECIAL_INODE_ROOT;
		} else if (strcmp(name,SPECIAL_FILE_NAME_META_UNDEL)==0) {
			inode = SPECIAL_INODE_META_UNDEL;
		} else {
			inode = metadataNameToInode(name);
			if (inode>0) {
				int status;
				Attributes attr;
				status = fs_getdetachedattr(inode,attr);
				status = saunafs_error_conv(status);
				if (status!=0) {
					fuse_reply_err(req, status);
				} else {
					e.ino = inode;
					e.attr_timeout = attr_cache_timeout;
					e.entry_timeout = entry_cache_timeout;
					sfs_attr_to_stat(inode ,attr,&e.attr);
					fuse_reply_entry(req,&e);
				}
				return;
			}
		}
		break;
	case SPECIAL_INODE_META_UNDEL:
		if (strcmp(name,".")==0) {
			inode = SPECIAL_INODE_META_UNDEL;
		} else if (strcmp(name,"..")==0) {
			inode = SPECIAL_INODE_META_TRASH;
		}
		break;
	case SPECIAL_INODE_META_RESERVED:
		if (strcmp(name,".")==0) {
			inode = SPECIAL_INODE_META_RESERVED;
		} else if (strcmp(name,"..")==0) {
			inode = SPECIAL_INODE_ROOT;
		} else {
			inode = metadataNameToInode(name);
			if (inode>0) {
				int status;
				Attributes attr;
				status = fs_getdetachedattr(inode,attr);
				status = saunafs_error_conv(status);
				if (status!=0) {
					fuse_reply_err(req, status);
				} else {
					e.ino = inode;
					e.attr_timeout = attr_cache_timeout;
					e.entry_timeout = entry_cache_timeout;
					sfs_attr_to_stat(inode ,attr,&e.attr);
					fuse_reply_entry(req,&e);
				}
				return;
			}
		}
		break;
	}
	if (inode==0) {
		fuse_reply_err(req,ENOENT);
	} else {
		e.ino = inode;
		e.attr_timeout = attr_cache_timeout;
		e.entry_timeout = entry_cache_timeout;
		sfsMetaStat(inode,&e.attr);
		fuse_reply_entry(req,&e);
	}
}

void sfs_meta_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
	struct stat o_stbuf;
	(void)fi;
	if (ino==SPECIAL_INODE_MASTERINFO) {
		memset(&o_stbuf, 0, sizeof(struct stat));
		o_stbuf = getMasterInfoStat();
		fuse_reply_attr(req, &o_stbuf, 3600.0);
	} else if (IS_SPECIAL_INODE(ino)) {
		memset(&o_stbuf, 0, sizeof(struct stat));
		sfsMetaStat(ino,&o_stbuf);
		fuse_reply_attr(req, &o_stbuf, attr_cache_timeout);
	} else {
		int status;
		Attributes attr;
		status = fs_getdetachedattr(ino, attr);
		status = saunafs_error_conv(status);
		if (status!=0) {
			fuse_reply_err(req, status);
		} else {
			memset(&o_stbuf, 0, sizeof(struct stat));
			sfs_attr_to_stat(ino, attr, &o_stbuf);
			fuse_reply_attr(req, &o_stbuf, attr_cache_timeout);
		}
	}
}

void sfs_meta_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *stbuf, int to_set, struct fuse_file_info *fi) {
	(void)to_set;
	(void)stbuf;
	sfs_meta_getattr(req,ino,fi);
}

void sfs_meta_unlink(fuse_req_t req, fuse_ino_t parent, const char *name) {
	int status;
	inode_t inode;
	if (parent!=SPECIAL_INODE_META_TRASH) {
		fuse_reply_err(req,EACCES);
		return;
	}
	inode = metadataNameToInode(name);
	if (inode==0) {
		fuse_reply_err(req,ENOENT);
		return;
	}
	status = fs_purge(inode);
	status = saunafs_error_conv(status);
	fuse_reply_err(req, status);
}

void sfs_meta_rename(fuse_req_t req, fuse_ino_t parent, const char *name, fuse_ino_t newparent, const char *newname, unsigned int flags) {
	(void)flags;
	int status;
	inode_t inode;
	(void)newname;
	if (parent!=SPECIAL_INODE_META_TRASH && newparent!=SPECIAL_INODE_META_UNDEL) {
		fuse_reply_err(req,EACCES);
		return;
	}
	inode = metadataNameToInode(name);
	if (inode==0) {
		fuse_reply_err(req,ENOENT);
		return;
	}
	status = fs_undel(inode);
	status = saunafs_error_conv(status);
	fuse_reply_err(req, status);
}

static uint32_t getDirMetaEntriesSize(inode_t ino) {
	// 2 could be name length + type
	constexpr uint32_t kFixedEntrySize = kinode_t_size + 2;

	switch (ino) {
	case SPECIAL_INODE_ROOT:
		return (4 * kFixedEntrySize) + 1 + 2 + strlen(SPECIAL_FILE_NAME_META_TRASH) +
		       strlen(SPECIAL_FILE_NAME_META_RESERVED);
	case SPECIAL_INODE_META_TRASH:
		return (3 * kFixedEntrySize) + 1 + 2 + strlen(SPECIAL_FILE_NAME_META_UNDEL);
	case SPECIAL_INODE_META_UNDEL:
		return (2 * kFixedEntrySize) + 1 + 2;
	case SPECIAL_INODE_META_RESERVED:
		return (2 * kFixedEntrySize) + 1 + 2;
	default:
		return 0;
	}

	return 0;
}

static void fillDirMetaEntries(uint8_t *buff, inode_t ino) {
	uint8_t nameLength;
	switch (ino) {
	case SPECIAL_INODE_ROOT:
		// .
		put8bit(&buff, 1);
		put8bit(&buff, '.');
		putINode(&buff, SPECIAL_INODE_ROOT);
		put8bit(&buff, TYPE_DIRECTORY);
		// ..
		put8bit(&buff, 2);
		put8bit(&buff, '.');
		put8bit(&buff, '.');
		putINode(&buff, SPECIAL_INODE_ROOT);
		put8bit(&buff, TYPE_DIRECTORY);
		// trash
		nameLength = strlen(SPECIAL_FILE_NAME_META_TRASH);
		put8bit(&buff, nameLength);
		memcpy(buff, SPECIAL_FILE_NAME_META_TRASH, nameLength);
		buff += nameLength;
		putINode(&buff, SPECIAL_INODE_META_TRASH);
		put8bit(&buff, TYPE_DIRECTORY);
		// reserved
		nameLength = strlen(SPECIAL_FILE_NAME_META_RESERVED);
		put8bit(&buff, nameLength);
		memcpy(buff, SPECIAL_FILE_NAME_META_RESERVED, nameLength);
		buff += nameLength;
		putINode(&buff, SPECIAL_INODE_META_RESERVED);
		put8bit(&buff, TYPE_DIRECTORY);
		return;
	case SPECIAL_INODE_META_TRASH:
		// .
		put8bit(&buff, 1);
		put8bit(&buff, '.');
		putINode(&buff, SPECIAL_INODE_META_TRASH);
		put8bit(&buff, TYPE_DIRECTORY);
		// ..
		put8bit(&buff, 2);
		put8bit(&buff, '.');
		put8bit(&buff, '.');
		putINode(&buff, SPECIAL_INODE_ROOT);
		put8bit(&buff, TYPE_DIRECTORY);
		// undel
		nameLength = strlen(SPECIAL_FILE_NAME_META_UNDEL);
		put8bit(&buff, nameLength);
		memcpy(buff, SPECIAL_FILE_NAME_META_UNDEL, nameLength);
		buff += nameLength;
		putINode(&buff, SPECIAL_INODE_META_UNDEL);
		put8bit(&buff, TYPE_DIRECTORY);
		return;
	case SPECIAL_INODE_META_UNDEL:
		// .
		put8bit(&buff, 1);
		put8bit(&buff, '.');
		putINode(&buff, SPECIAL_INODE_META_UNDEL);
		put8bit(&buff, TYPE_DIRECTORY);
		// ..
		put8bit(&buff, 2);
		put8bit(&buff, '.');
		put8bit(&buff, '.');
		putINode(&buff, SPECIAL_INODE_META_TRASH);
		put8bit(&buff, TYPE_DIRECTORY);
		return;
	case SPECIAL_INODE_META_RESERVED:
		// .
		put8bit(&buff, 1);
		put8bit(&buff, '.');
		putINode(&buff, SPECIAL_INODE_META_RESERVED);
		put8bit(&buff, TYPE_DIRECTORY);
		// ..
		put8bit(&buff, 2);
		put8bit(&buff, '.');
		put8bit(&buff, '.');
		putINode(&buff, SPECIAL_INODE_ROOT);
		put8bit(&buff, TYPE_DIRECTORY);
		return;
	}
}

static uint32_t getDirDataEntriesSize(const uint8_t *dataBuffer, size_t dataSize) {
	uint8_t nameLength;
	uint32_t totalSize = 0;
	const uint8_t *eptr;

	if (dataBuffer == nullptr || dataSize == 0) { return 0; }

	eptr = dataBuffer + dataSize;
	while (dataBuffer < eptr) {
		nameLength = dataBuffer[0];
		dataBuffer += kDirEntryMetaSize + nameLength;
		if (nameLength > NAME_MAX - kDirEntryExtraFieldsSize) {
			totalSize += kDirEntryStride + NAME_MAX;
		} else {
			totalSize += kDirEntryStride + nameLength + kDirEntryExtraFieldsSize;
		}
	}

	return totalSize;
}

static void convertDirDataEntries(uint8_t *buff, const uint8_t *dataBuffer, uint32_t dataSize) {
	const char *name;
	inode_t inode;
	uint8_t nameLength;
	uint8_t inodeLength;
	const uint8_t *eptr;
	eptr = dataBuffer + dataSize;

	while (dataBuffer < eptr) {
		nameLength = dataBuffer[0];

		if (dataBuffer + nameLength + kDirEntryMetaSize <= eptr) {
			dataBuffer++;

			if (nameLength > NAME_MAX - kDirEntryExtraFieldsSize) {
				inodeLength = NAME_MAX;
			} else {
				inodeLength = nameLength + kDirEntryExtraFieldsSize;
			}

			put8bit(&buff, inodeLength);
			name = (const char *)dataBuffer;
			dataBuffer += nameLength;
			getINode(&dataBuffer, inode);
			sprintf((char *)buff, "%08" PRIXiNode "|", inode);

			if (nameLength > NAME_MAX - kDirEntryExtraFieldsSize) {
				memcpy(buff + kDirEntryExtraFieldsSize, name, NAME_MAX - kDirEntryExtraFieldsSize);
				buff += NAME_MAX;
			} else {
				memcpy(buff + kDirEntryExtraFieldsSize, name, nameLength);
				buff += kDirEntryExtraFieldsSize + nameLength;
			}

			putINode(&buff, inode);
			put8bit(&buff, TYPE_FILE);
		} else {
			safs::log_warn("dir data malformed (trash)");
			dataBuffer = eptr;
		}
	}
}

static void fillDirectoryBufferMeta(DirectoryBuffer *dirbuf, inode_t ino) {
	int status;
	uint32_t metaEntriesSize, entriesSize = 0, dataEntriesSize;
	const uint8_t *bufData = nullptr;

	dirbuf->buffer.clear();
	dirbuf->size = 0;
	metaEntriesSize = getDirMetaEntriesSize(ino);

	if (ino == SPECIAL_INODE_META_TRASH) {
		status = fs_gettrash(&bufData, &entriesSize);
		if (status != SAUNAFS_STATUS_OK) return;
		dataEntriesSize = getDirDataEntriesSize(bufData, entriesSize);
	} else if (ino == SPECIAL_INODE_META_RESERVED) {
		status = fs_getreserved(&bufData, &entriesSize);
		if (status != SAUNAFS_STATUS_OK) return;
		dataEntriesSize = getDirDataEntriesSize(bufData, entriesSize);
	} else {
		dataEntriesSize = 0;
	}

	if (metaEntriesSize + dataEntriesSize == 0) { return; }

	try {
		dirbuf->buffer.resize(metaEntriesSize + dataEntriesSize);
	} catch (const std::bad_alloc &) {
		safs::log_critical("out of memory");
		return;
	}

	if (metaEntriesSize > 0) { fillDirMetaEntries(dirbuf->buffer.data(), ino); }

	if (dataEntriesSize > 0) {
		convertDirDataEntries(dirbuf->buffer.data() + metaEntriesSize, bufData,
		                        entriesSize);
	}

	dirbuf->size = metaEntriesSize + dataEntriesSize;
}

void sfs_meta_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
	constexpr auto isValidMetaMountInode = [](fuse_ino_t i) {
		return i == SPECIAL_INODE_ROOT || i == SPECIAL_INODE_META_TRASH ||
		       i == SPECIAL_INODE_META_UNDEL || i == SPECIAL_INODE_META_RESERVED;
	};

	if (isValidMetaMountInode(ino)) {
		auto dirinfo = std::make_unique<DirectoryBuffer>();
		dirinfo->buffer.clear();
		dirinfo->size = 0;
		dirinfo->wasRead = false;
		fi->fh = reinterpret_cast<uintptr_t>(dirinfo.release());

		if (fuse_reply_open(req, fi) == -ENOENT) {
			fi->fh = 0;
		}
	} else {
		fuse_reply_err(req, ENOTDIR);
	}
}

void replyDirReadRoot(fuse_req_t request, off_t offset, size_t maxsize) {
	std::array<char, READDIR_BUFFSIZE> buffer{};
	struct stat statBuffer{};
	static const auto files = rootDirEntries();
	size_t size = 0;

	sassert(offset >= 0);

	if (static_cast<size_t>(offset) > files.size()) {
		fuse_reply_buf(request, nullptr, 0);
		return;
	}

	for (const auto &file : std::span(files).subspan(offset)) {
		offset += 1;
		resetStat(file.inode, file.type, statBuffer);
		size_t needed = fuse_add_direntry(request, nullptr, 0, file.name.c_str(), &statBuffer, 0);
		if (size + needed > buffer.size() || size + needed > maxsize) {
			// No more buffer space or we would be over maxsize
			fuse_reply_buf(request, buffer.data(), size);
			return;
		}

		fuse_add_direntry(request, buffer.data() + size, buffer.size() - size,
		                                  file.name.c_str(), &statBuffer, offset);
		size += needed;
	}
	fuse_reply_buf(request, buffer.data(), size);
}

void fillDirEntryBuffer(fuse_req_t req, DirectoryBuffer *dirinfo, off_t &off, char *dirEntryBuffer,
                        size_t &size, size_t &writePos) {
	char *entryName;
	char nameTerminator;
	size_t entryLength;
	inode_t inode;
	uint8_t type;
	struct stat stbuf;
	uint8_t nameLength;
	uint8_t done = 0;

	size = std::min<size_t>(size, READDIR_BUFFSIZE);
	const uint8_t *entryPtr = (const uint8_t *)(dirinfo->buffer.data()) + off;
	const uint8_t *endPtr = (const uint8_t *)(dirinfo->buffer.data()) + dirinfo->size;

	while (entryPtr < endPtr && done == 0) {
		nameLength = entryPtr[0];
		entryPtr++;
		entryName = (char *)entryPtr;
		entryPtr += nameLength;
		off += nameLength + kDirEntryStride;
		if (entryPtr + kDirEntryMetaSize <= endPtr) {
			getINode(&entryPtr, inode);
			type = get8bit(&entryPtr);
			resetStat(inode, type, stbuf);
			nameTerminator = entryName[nameLength];
			entryName[nameLength] = 0;
			entryLength = fuse_add_direntry(req, dirEntryBuffer + writePos, size - writePos,
			                                entryName, &stbuf, off);
			entryName[nameLength] = nameTerminator;
			if (writePos + entryLength > size) {
				done = 1;
			} else {
				writePos += entryLength;
			}
		}
	}
}

void sfs_meta_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
                      struct fuse_file_info *fi) {
	auto dirinfo = reinterpret_cast<DirectoryBuffer *>(fi->fh);
	if (dirinfo == nullptr) {
		fuse_reply_err(req, ENOENT);
		return;
	}

	if (off < 0) {
		fuse_reply_err(req, EINVAL);
		return;
	}
	std::lock_guard lock(dirinfo->lock);
	if (!dirinfo->wasRead || (dirinfo->wasRead && off == 0)) {
		if (ino == SPECIAL_INODE_ROOT) {
			replyDirReadRoot(req, off, size);
			return;
		}
		fillDirectoryBufferMeta(dirinfo, ino);
	}
	dirinfo->wasRead = true;

	if (off >= (off_t)(dirinfo->size)) {
		fuse_reply_buf(req, nullptr, 0);
	} else {
		char dirEntryBuffer[READDIR_BUFFSIZE];
		size_t writePos = 0;
		fillDirEntryBuffer(req, dirinfo, off, dirEntryBuffer, size, writePos);
		fuse_reply_buf(req, dirEntryBuffer, writePos);
	}
}

void sfs_meta_releasedir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
	(void)ino;
	auto dirinfo = std::unique_ptr<DirectoryBuffer>(reinterpret_cast<DirectoryBuffer *>(fi->fh));
	if (dirinfo) {
		dirinfo->lock.lock();
		dirinfo->lock.unlock();
		fi->fh = 0;
	}
	fuse_reply_err(req, 0);
}

void sfs_meta_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
	auto pathinfo = std::make_unique<PathBuffer>();
	const uint8_t *path;
	int status;

	if (ino == SPECIAL_INODE_MASTERINFO) {
		fi->fh = 0;
		fi->direct_io = 0;
		fi->keep_cache = 1;
		fuse_reply_open(req, fi);
		return;
	}

	if (IS_SPECIAL_INODE(ino)) {
		fuse_reply_err(req, EACCES);
		return;
	}

	status = fs_gettrashpath(ino, &path);
	status = saunafs_error_conv(status);

	if (status) {
		fuse_reply_err(req, status);
	} else {
		pathinfo->changed = false;
		pathinfo->size = strlen((char *)path) + 1;
		pathinfo->path = std::string((char *)path);
		pathinfo->path.append("\n"); 
		fi->direct_io = 1;
		fi->fh = reinterpret_cast<uintptr_t>(pathinfo.release());

		if (fuse_reply_open(req, fi) == -ENOENT) { fi->fh = 0; }
	}
}

void sfs_meta_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
	if (ino == SPECIAL_INODE_MASTERINFO) {
		fuse_reply_err(req, 0);
		return;
	}

	auto pathinfo = std::unique_ptr<PathBuffer>(reinterpret_cast<PathBuffer *>(fi->fh));

	std::unique_lock lock(pathinfo->lock);
	if (pathinfo->changed) {
		if (pathinfo->path.back() == '\n') {
			pathinfo->path.pop_back();
		} else {
			pathinfo->path.resize(pathinfo->size + 1);
			pathinfo->path.back() = '\0';
		}
		fs_settrashpath(ino, reinterpret_cast<const uint8_t *>(pathinfo->path.c_str()));
	}
	lock.unlock();

	fi->fh = 0;
	fuse_reply_err(req, 0);
}

void sfs_meta_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
                   struct fuse_file_info *fi) {
#ifdef MASTERINFO_WITH_VERSION
	constexpr off_t kMasterInfoSize =
	    14;  // size of the master info buffer with version information.
#else
	constexpr off_t kMasterInfoSize =
	    10;  // size of the master info buffer without version information.
#endif
	auto pathinfo = reinterpret_cast<PathBuffer *>(fi->fh);

	if (ino == SPECIAL_INODE_MASTERINFO) {
		uint8_t masterinfo[14];
		fs_getmasterlocation(masterinfo);
		masterproxy_getlocation(masterinfo);

		if (off >= kMasterInfoSize) {
			fuse_reply_buf(req, nullptr, 0);
		} else if (off + size > kMasterInfoSize) {
			fuse_reply_buf(req, (char *)(masterinfo + off), kMasterInfoSize - off);
		} else {
			fuse_reply_buf(req, (char *)(masterinfo + off), size);
		}
		return;
	}

	if (pathinfo == nullptr) {
		fuse_reply_err(req, EBADF);
		return;
	}

	std::unique_lock lock(pathinfo->lock);
	if (off < 0) {
		lock.unlock();
		fuse_reply_err(req, EINVAL);
		return;
	}

	if ((size_t)off > pathinfo->size) {
		fuse_reply_buf(req, nullptr, 0);
	} else if (off + size > pathinfo->size) {
		fuse_reply_buf(req, (pathinfo->path.c_str()) + off, (pathinfo->size) - off);
	} else {
		fuse_reply_buf(req, (pathinfo->path.c_str()) + off, size);
	}
}

void sfs_meta_write(fuse_req_t req, fuse_ino_t ino, const char *buf, size_t size, off_t off,
                    struct fuse_file_info *fi) {
	auto pathinfo = reinterpret_cast<PathBuffer *>(fi->fh);
	if (ino == SPECIAL_INODE_MASTERINFO) {
		fuse_reply_err(req, EACCES);
		return;
	}

	if (pathinfo == nullptr) {
		fuse_reply_err(req, EBADF);
		return;
	}

	if (off + size > PATH_SIZE_LIMIT) {
		fuse_reply_err(req, EINVAL);
		return;
	}

	std::unique_lock lock(pathinfo->lock);
	if (pathinfo->changed == 0) { pathinfo->size = 0; }

	if (off + size > pathinfo->size) {
		pathinfo->size = off + size;
		pathinfo->path.resize(off + size, '\0');
	}

	std::memcpy(pathinfo->path.data() + off, buf, size);
	pathinfo->changed = 1;
	lock.unlock();
	fuse_reply_write(req, size);
}

void sfs_meta_init(double entry_cache_timeout_in, double attr_cache_timeout_in) {
	entry_cache_timeout = entry_cache_timeout_in;
	attr_cache_timeout = attr_cache_timeout_in;
	fmt::print(stderr,
	           "cache parameters: entry_cache_timeout={:.2f} "
	           "attr_cache_timeout={:.2f}\n",
	           entry_cache_timeout, attr_cache_timeout);
}

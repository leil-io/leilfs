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
#include "common/saunafs_version.h"
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

// Number of bytes used for metadata fields (inode + file type) after the name in a directory
constexpr size_t kDirEntryMetaSize = sizeof(inode_t) + 1;

// Total number of bytes reserved at the start of a directory entry for extra fields,
// such as a formatted inode string and separator before the name. This includes:
//  - 2 * sizeof(inode_t) for hexadecimal inode string representation
//  - 1 character for the '|' separator
constexpr size_t kDirEntryHexPreffixSize = 2 * sizeof(inode_t) + 1;

// Total stride (name length + metadata) to advance to the next directory entry
constexpr size_t kDirEntryStride = kDirEntryMetaSize + 1;

// Block size used for filesystem statistics and block calculations (in bytes)
constexpr size_t kBlockSize = 512;

// Maximum file permission bits (all permission bits, sticky/setuid/setgid)
constexpr mode_t kMaxFilePermissions = 07777;

// Maximum number of requested entries to buffer in a trash/reserved directory
// reading operation. This is used to limit the number of entries
// processed at once, to avoid excessive memory usage.
constexpr uint64_t kMaxEntriesToCache = 100000;

constexpr uint64_t kStatfsFilesBase = 1'000'000'000 + PKGVERSION;

struct DirectoryBuffer {
	bool wasRead = false;
	std::vector<uint8_t> buffer;
	std::mutex lock;
};

struct PathBuffer {
	bool changed;
	std::string path;
	std::mutex lock;
};

#define IS_SPECIAL_INODE(inode) ((inode)>=SPECIAL_INODE_BASE || (inode)==SPECIAL_INODE_ROOT)

static double entry_cache_timeout = 0.0;
static double attr_cache_timeout = 1.0;

static void sfs_attr_to_stat(inode_t inode, const Attributes &attr, struct stat *stbuf) {
	uint16_t attrmode;
	uint8_t attrtype;
	uint32_t attruid, attrgid, attratime, attrmtime, attrctime, attrnlink;
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

	if (attrtype == TYPE_FILE || attrtype == TYPE_TRASH || attrtype == TYPE_RESERVED) {
		stbuf->st_mode = S_IFREG | (attrmode & kMaxFilePermissions);
	} else {
		stbuf->st_mode = 0;
	}

	stbuf->st_size = attrlength;
	stbuf->st_blocks = (attrlength + kBlockSize - 1) / kBlockSize;
	stbuf->st_uid = attruid;
	stbuf->st_gid = attrgid;
	stbuf->st_atime = attratime;
	stbuf->st_mtime = attrmtime;
	stbuf->st_ctime = attrctime;
	stbuf->st_nlink = attrnlink;
}

void sfs_meta_statfs(fuse_req_t req, fuse_ino_t ino) {
	uint64_t totalspace, availspace, trashspace, reservedspace;
	inode_t inodes;
	struct statvfs stfsbuf;
	memset(&stfsbuf, 0, sizeof(stfsbuf));

	(void)ino;
	fs_statfs(&totalspace, &availspace, &trashspace, &reservedspace, &inodes);

	stfsbuf.f_namemax = NAME_MAX;
	stfsbuf.f_frsize = kBlockSize;
	stfsbuf.f_bsize = kBlockSize;
	stfsbuf.f_blocks = trashspace / kBlockSize + reservedspace / kBlockSize;
	stfsbuf.f_bfree = reservedspace / kBlockSize;
	stfsbuf.f_bavail = reservedspace / kBlockSize;
	stfsbuf.f_files = kStatfsFilesBase;
	stfsbuf.f_ffree = kStatfsFilesBase;
	stfsbuf.f_favail = kStatfsFilesBase;

	fuse_reply_statfs(req, &stfsbuf);
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

static uint32_t getDirDataEntriesSize(const std::vector<NamedInodeEntry> &entries) {
	uint8_t nameLength;
	uint32_t totalSize = 0;

	if (entries.empty()) { return 0; }

	for (const auto &entry : entries) {
		nameLength = entry.name.size();
		if (nameLength > NAME_MAX - kDirEntryHexPreffixSize) {
			totalSize += kDirEntryStride + NAME_MAX;
		} else {
			totalSize += kDirEntryStride + nameLength + kDirEntryHexPreffixSize;
		}
	}

	return totalSize;
}

static void convertDirDataEntries(uint8_t *buff, const std::vector<NamedInodeEntry> &entries) {
	const uint8_t *name{};
	uint8_t nameLength{};
	uint8_t inodeLength{};

	for (const auto &entry : entries) {
		nameLength = entry.name.size();

		if (nameLength > NAME_MAX - kDirEntryHexPreffixSize) {
			inodeLength = NAME_MAX;
		} else {
			inodeLength = nameLength + kDirEntryHexPreffixSize;
		}

		put8bit(&buff, inodeLength);
		name = reinterpret_cast<const uint8_t *>(entry.name.data());
		sprintf((char *)buff, "%0*" PRIXiNode "|", 2 * (int)sizeof(inode_t), entry.inode);

		uint8_t sizeOfTrailingNamePiece =
		    std::min(nameLength, static_cast<uint8_t>(NAME_MAX - kDirEntryHexPreffixSize));
		memcpy(buff + kDirEntryHexPreffixSize, name, sizeOfTrailingNamePiece);

		// Replace '/' with '|' in the name part of the entry
		for (size_t i = 0; i < sizeOfTrailingNamePiece; i++) {
			if (buff[kDirEntryHexPreffixSize + i] == '/') {
				buff[kDirEntryHexPreffixSize + i] = '|';
			}
		}

		buff += sizeOfTrailingNamePiece + kDirEntryHexPreffixSize;
		putINode(&buff, entry.inode);
		put8bit(&buff, TYPE_FILE);
	}
}

void sfs_meta_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
	constexpr auto isValidMetaMountInode = [](inode_t inode) {
		return inode == SPECIAL_INODE_ROOT || inode == SPECIAL_INODE_META_TRASH ||
		       inode == SPECIAL_INODE_META_UNDEL || inode == SPECIAL_INODE_META_RESERVED;
	};

	if (isValidMetaMountInode(ino)) {
		auto *dirinfo = new DirectoryBuffer();
		dirinfo->buffer.clear();
		dirinfo->wasRead = false;
		fi->fh = reinterpret_cast<uintptr_t>(dirinfo);

		if (fuse_reply_open(req, fi) != 0) {
			delete dirinfo;
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
	const uint8_t *entryPtr = reinterpret_cast<const uint8_t *>(dirinfo->buffer.data()) + off;
	const uint8_t *endPtr =
	    reinterpret_cast<const uint8_t *>(dirinfo->buffer.data()) + dirinfo->buffer.size();

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

void getDirectoryEntriesLimited(DirectoryBuffer *dirinfo, inode_t ino, off_t byteOffset,
                                size_t maxByteSize) {
	uint8_t status = 0;
	uint32_t entriesSize = 0;
	uint32_t metaEntriesSize = 0;
	uint32_t dataEntriesSize = 0;
	std::vector<uint8_t> bufData;

	// Check if the directory has already been read and cached
	if (dirinfo->wasRead) { return; }

	if (byteOffset == 0) {
		metaEntriesSize = getDirMetaEntriesSize(ino);
		if (metaEntriesSize > 0 && byteOffset < metaEntriesSize) {
			std::vector<uint8_t> metaBuf(metaEntriesSize);
			fillDirMetaEntries(metaBuf.data(), ino);
			size_t start = byteOffset;
			size_t toCopy = std::min(maxByteSize, metaEntriesSize - start);
			bufData.insert(bufData.end(), metaBuf.begin() + start,
			               metaBuf.begin() + start + toCopy);
		}
	}

	// Offset is in data entries
	entriesSize = kMaxEntriesToCache;

	std::vector<NamedInodeEntry> entries;
	if (ino == SPECIAL_INODE_META_TRASH) {
		status = fs_gettrash(0, entriesSize, entries);
	} else if (ino == SPECIAL_INODE_META_RESERVED) {
		status = fs_getreserved(0, entriesSize, entries);
	}

	dataEntriesSize = getDirDataEntriesSize(entries);

	if (status != SAUNAFS_STATUS_OK || dataEntriesSize == 0) {
		dirinfo->buffer = bufData;
		return;
	}

	bufData.resize(dataEntriesSize + (byteOffset == 0 ? metaEntriesSize : 0));
	convertDirDataEntries(bufData.data() + (byteOffset == 0 ? metaEntriesSize : 0), entries);

	dirinfo->buffer = bufData;
	dirinfo->wasRead = true;
}

void sfs_meta_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
                      struct fuse_file_info *fi) {
	auto *dirinfo = reinterpret_cast<DirectoryBuffer *>(fi->fh);
	if (dirinfo == nullptr) {
		fuse_reply_err(req, ENOENT);
		return;
	}

	if (off < 0) {
		fuse_reply_err(req, EINVAL);
		return;
	}

	std::lock_guard lock(dirinfo->lock);
	if (ino == SPECIAL_INODE_ROOT) {
		replyDirReadRoot(req, off, size);
		return;
	}

	getDirectoryEntriesLimited(dirinfo, ino, off, size);

	if (dirinfo->buffer.empty() || size == 0) {
		fuse_reply_buf(req, nullptr, 0);
		return;
	}

	char dirEntryBuffer[READDIR_BUFFSIZE];
	size_t writePos = 0;
	fillDirEntryBuffer(req, dirinfo, off, dirEntryBuffer, size, writePos);
	fuse_reply_buf(req, dirEntryBuffer, writePos);
}

void sfs_meta_releasedir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
	(void)ino;
	auto *dirinfo = reinterpret_cast<DirectoryBuffer *>(fi->fh);
	dirinfo->lock.lock();
	dirinfo->lock.unlock();
	delete dirinfo;
	fi->fh = 0;
	fuse_reply_err(req, 0);
}

void sfs_meta_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
	auto *pathinfo = new PathBuffer();
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
		pathinfo->path = std::string(reinterpret_cast<const char *>(path));
		pathinfo->path.append("\n");
		fi->direct_io = 1;
		fi->fh = reinterpret_cast<uintptr_t>(pathinfo);

		if (fuse_reply_open(req, fi) != 0) {
			delete pathinfo;
			fi->fh = 0;
		}
	}
}

void sfs_meta_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
	if (ino == SPECIAL_INODE_MASTERINFO) {
		fuse_reply_err(req, 0);
		return;
	}

	auto *pathinfo = reinterpret_cast<PathBuffer *>(fi->fh);

	std::unique_lock lock(pathinfo->lock);
	if (pathinfo->changed) {
		if (pathinfo->path.back() == '\n') {
			pathinfo->path.pop_back();
		} else {
			pathinfo->path.resize(pathinfo->path.size() + 1);
			pathinfo->path.back() = '\0';
		}
		fs_settrashpath(ino, reinterpret_cast<const uint8_t *>(pathinfo->path.c_str()));
	}
	lock.unlock();
	delete pathinfo;
	fi->fh = 0;
	fuse_reply_err(req, 0);
}

void sfs_meta_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
                   struct fuse_file_info *fi) {
#ifdef MASTERINFO_WITH_VERSION
	// size of the master info buffer with version information.
	constexpr off_t kMasterInfoSize = 14;
#else
	// size of the master info buffer without version information.
	constexpr off_t kMasterInfoSize = 10;
#endif
	auto * pathinfo = reinterpret_cast<PathBuffer *>(fi->fh);

	if (ino == SPECIAL_INODE_MASTERINFO) {
		uint8_t masterinfo[14];
		fs_getmasterlocation(masterinfo);
		masterproxy_getlocation(masterinfo);

		if (off >= kMasterInfoSize) {
			fuse_reply_buf(req, nullptr, 0);
		} else if (off + size > kMasterInfoSize) {
			fuse_reply_buf(req, reinterpret_cast<char *>(masterinfo + off), kMasterInfoSize - off);
		} else {
			fuse_reply_buf(req, reinterpret_cast<char *>(masterinfo + off), size);
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

	if ((size_t)off > pathinfo->path.size()) {
		fuse_reply_buf(req, nullptr, 0);
	} else if (off + size > pathinfo->path.size()) {
		fuse_reply_buf(req, (pathinfo->path.c_str()) + off, (pathinfo->path.size()) - off);
	} else {
		fuse_reply_buf(req, (pathinfo->path.c_str()) + off, size);
	}
}

void sfs_meta_write(fuse_req_t req, fuse_ino_t ino, const char *buf, size_t size, off_t off,
                    struct fuse_file_info *fi) {
	auto * pathinfo = reinterpret_cast<PathBuffer *>(fi->fh);
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
	if (pathinfo->changed == false) { pathinfo->path.clear(); }

	if (off + size > pathinfo->path.size()) {
		pathinfo->path.resize(off + size, '\0');
	}

	std::memcpy(pathinfo->path.data() + off, buf, size);
	pathinfo->changed = true;
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

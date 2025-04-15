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

#include "common/massert.h"
#include "common/platform.h"
#include "mount/fuse/sfs_meta_fuse.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fuse_lowlevel.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <span>

#include "common/datapack.h"
#include "common/type_defs.h"
#include "protocol/SFSCommunication.h"
#include "slogger/slogger.h"
#include "common/special_inode_defs.h"
#include "mount/exports.h"
#include "mount/mastercomm.h"
#include "mount/masterproxy.h"

static_assert(FUSE_ROOT_ID == SPECIAL_INODE_ROOT, "invalid value of FUSE_ROOT_ID");

struct dirbuf {
	bool wasread;
	uint8_t *p;
	size_t size;
	pthread_mutex_t lock;
};

typedef struct _pathbuf {
	int changed;
	char *p;
	size_t size;
	pthread_mutex_t lock;
} pathbuf;

#define READDIR_BUFFSIZE 50000

#define NAME_MAX 255
#define PATH_SIZE_LIMIT 1024

#define META_ROOT_MODE 0555

// 0x0124 = 0444
#ifdef MASTERINFO_WITH_VERSION
static Attributes masterinfoattr={{'f', 0x01,0x24, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1, 0,0,0,0,0,0,0,14}};
#else
static Attributes masterinfoattr={{'f', 0x01,0x24, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1, 0,0,0,0,0,0,0,10}};
#endif

#define PKGVERSION \
		((SAUNAFS_PACKAGE_VERSION_MAJOR)*1000000 + \
		(SAUNAFS_PACKAGE_VERSION_MINOR)*1000 + \
		(SAUNAFS_PACKAGE_VERSION_MICRO))

#define IS_SPECIAL_INODE(inode) ((inode)>=SPECIAL_INODE_BASE || (inode)==SPECIAL_INODE_ROOT)

static int debug_mode = 0;
static double entry_cache_timeout = 0.0;
static double attr_cache_timeout = 1.0;

inode_t sfs_meta_name_to_inode(const char *name) {
	inode_t inode=0;
	char *end;
	inode = strtoul(name,&end,16);
	if (*end=='|' && end[1]!=0) {
		return inode;
	} else {
		return 0;
	}
}

static void sfs_meta_type_to_stat(inode_t inode,uint8_t type, struct stat *stbuf) {
	memset(stbuf,0,sizeof(struct stat));
	stbuf->st_ino = inode;
	switch (type) {
	case TYPE_DIRECTORY:
		stbuf->st_mode = S_IFDIR;
		break;
	case TYPE_SYMLINK:
		stbuf->st_mode = S_IFLNK;
		break;
	case TYPE_FILE:
		stbuf->st_mode = S_IFREG;
		break;
	case TYPE_FIFO:
		stbuf->st_mode = S_IFIFO;
		break;
	case TYPE_SOCKET:
		stbuf->st_mode = S_IFSOCK;
		break;
	case TYPE_BLOCKDEV:
		stbuf->st_mode = S_IFBLK;
		break;
	case TYPE_CHARDEV:
		stbuf->st_mode = S_IFCHR;
		break;
	default:
		stbuf->st_mode = 0;
	}
}

static void sfs_meta_stat(inode_t inode, struct stat *stbuf) {
	int now;
	stbuf->st_ino = inode;
	stbuf->st_size = 0;
	stbuf->st_blocks = 0;
	switch (inode) {
	case SPECIAL_INODE_ROOT:
		stbuf->st_nlink = 4;
		stbuf->st_mode = S_IFDIR | META_ROOT_MODE ;
		break;
	case SPECIAL_INODE_META_TRASH:
		stbuf->st_nlink = 3;
		stbuf->st_mode = S_IFDIR | (nonRootAllowedToUseMeta() ? 0777 : 0700) ;
		break;
	case SPECIAL_INODE_META_UNDEL:
		stbuf->st_nlink = 2;
		stbuf->st_mode = S_IFDIR | (nonRootAllowedToUseMeta() ? 0777 : 0200) ;
		break;
	case SPECIAL_INODE_META_RESERVED:
		stbuf->st_nlink = 2;
		stbuf->st_mode = S_IFDIR | (nonRootAllowedToUseMeta() ? 0555 : 0500) ;
		break;
	}
	stbuf->st_uid = 0;
	stbuf->st_gid = 0;
	now = time(NULL);
	stbuf->st_atime = now;
	stbuf->st_mtime = now;
	stbuf->st_ctime = now;
}

static void sfs_attr_to_stat(inode_t inode, const Attributes &attr, struct stat *stbuf) {
	uint16_t attrmode;
	uint8_t attrtype;
	uint32_t attruid,attrgid,attratime,attrmtime,attrctime,attrnlink;
	uint64_t attrlength;
	const uint8_t *ptr;
	ptr = attr.data();
	attrtype = get8bit(&ptr);
	attrmode = get16bit(&ptr);
	attruid = get32bit(&ptr);
	attrgid = get32bit(&ptr);
	attratime = get32bit(&ptr);
	attrmtime = get32bit(&ptr);
	attrctime = get32bit(&ptr);
	attrnlink = get32bit(&ptr);
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
			sfs_attr_to_stat(SPECIAL_INODE_MASTERINFO,masterinfoattr,&e.attr);
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
			inode = sfs_meta_name_to_inode(name);
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
			inode = sfs_meta_name_to_inode(name);
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
		sfs_meta_stat(inode,&e.attr);
		fuse_reply_entry(req,&e);
	}
}

void sfs_meta_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
	struct stat o_stbuf;
	(void)fi;
	if (ino==SPECIAL_INODE_MASTERINFO) {
		memset(&o_stbuf, 0, sizeof(struct stat));
		sfs_attr_to_stat(ino,masterinfoattr,&o_stbuf);
		fuse_reply_attr(req, &o_stbuf, 3600.0);
	} else if (IS_SPECIAL_INODE(ino)) {
		memset(&o_stbuf, 0, sizeof(struct stat));
		sfs_meta_stat(ino,&o_stbuf);
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
			sfs_attr_to_stat(ino,attr,&o_stbuf);
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
	inode = sfs_meta_name_to_inode(name);
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
	inode = sfs_meta_name_to_inode(name);
	if (inode==0) {
		fuse_reply_err(req,ENOENT);
		return;
	}
	status = fs_undel(inode);
	status = saunafs_error_conv(status);
	fuse_reply_err(req, status);
}

static uint32_t dir_metaentries_size(inode_t ino) {
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

struct MetaStat {
	std::string name;
	inode_t inode;
	char type;
};

static constexpr std::array<MetaStat, 4> rootDirEntries() {
		auto rootDir = MetaStat(".", SPECIAL_INODE_ROOT, TYPE_DIRECTORY);
		auto upDir = MetaStat("..", SPECIAL_INODE_ROOT, TYPE_DIRECTORY);
		auto trash = MetaStat(SPECIAL_FILE_NAME_META_TRASH, SPECIAL_INODE_META_TRASH, TYPE_DIRECTORY);
		auto reserved = MetaStat(SPECIAL_FILE_NAME_META_RESERVED, SPECIAL_INODE_META_RESERVED, TYPE_DIRECTORY);
		std::array<MetaStat, 4> entries = {rootDir, upDir, trash, reserved};
		return entries;
}

struct MetaStat {
	std::string name;
	SaunaClient::Inode inode;
	char type;
};

static constexpr std::array<MetaStat, 4> rootDirEntries() {
		auto rootDir = MetaStat(".", SPECIAL_INODE_ROOT, TYPE_DIRECTORY);
		auto upDir = MetaStat("..", SPECIAL_INODE_ROOT, TYPE_DIRECTORY);
		auto trash = MetaStat(SPECIAL_FILE_NAME_META_TRASH, SPECIAL_INODE_META_TRASH, TYPE_DIRECTORY);
		auto reserved = MetaStat(SPECIAL_FILE_NAME_META_RESERVED, SPECIAL_INODE_META_RESERVED, TYPE_DIRECTORY);
		std::array<MetaStat, 4> entries = {rootDir, upDir, trash, reserved};
		return entries;
}

static void dir_metaentries_fill(uint8_t *buff, inode_t ino) {
	uint8_t l;
	switch (ino) {
	case SPECIAL_INODE_ROOT:
		// .
		put8bit(&buff,1);
		put8bit(&buff,'.');
		putINode(&buff,SPECIAL_INODE_ROOT);
		put8bit(&buff,TYPE_DIRECTORY);
		// ..
		put8bit(&buff,2);
		put8bit(&buff,'.');
		put8bit(&buff,'.');
		putINode(&buff,SPECIAL_INODE_ROOT);
		put8bit(&buff,TYPE_DIRECTORY);
		// trash
		l = strlen(SPECIAL_FILE_NAME_META_TRASH);
		put8bit(&buff,l);
		memcpy(buff,SPECIAL_FILE_NAME_META_TRASH,l);
		buff+=l;
		putINode(&buff,SPECIAL_INODE_META_TRASH);
		put8bit(&buff,TYPE_DIRECTORY);
		// reserved
		l = strlen(SPECIAL_FILE_NAME_META_RESERVED);
		put8bit(&buff,l);
		memcpy(buff,SPECIAL_FILE_NAME_META_RESERVED,l);
		buff+=l;
		putINode(&buff,SPECIAL_INODE_META_RESERVED);
		put8bit(&buff,TYPE_DIRECTORY);
		return;
	case SPECIAL_INODE_META_TRASH:
		// .
		put8bit(&buff,1);
		put8bit(&buff,'.');
		putINode(&buff,SPECIAL_INODE_META_TRASH);
		put8bit(&buff,TYPE_DIRECTORY);
		// ..
		put8bit(&buff,2);
		put8bit(&buff,'.');
		put8bit(&buff,'.');
		putINode(&buff,SPECIAL_INODE_ROOT);
		put8bit(&buff,TYPE_DIRECTORY);
		// undel
		l = strlen(SPECIAL_FILE_NAME_META_UNDEL);
		put8bit(&buff,l);
		memcpy(buff,SPECIAL_FILE_NAME_META_UNDEL,l);
		buff+=l;
		putINode(&buff,SPECIAL_INODE_META_UNDEL);
		put8bit(&buff,TYPE_DIRECTORY);
		return;
	case SPECIAL_INODE_META_UNDEL:
		// .
		put8bit(&buff,1);
		put8bit(&buff,'.');
		putINode(&buff,SPECIAL_INODE_META_UNDEL);
		put8bit(&buff,TYPE_DIRECTORY);
		// ..
		put8bit(&buff,2);
		put8bit(&buff,'.');
		put8bit(&buff,'.');
		putINode(&buff,SPECIAL_INODE_META_TRASH);
		put8bit(&buff,TYPE_DIRECTORY);
		return;
	case SPECIAL_INODE_META_RESERVED:
		// .
		put8bit(&buff,1);
		put8bit(&buff,'.');
		putINode(&buff,SPECIAL_INODE_META_RESERVED);
		put8bit(&buff,TYPE_DIRECTORY);
		// ..
		put8bit(&buff,2);
		put8bit(&buff,'.');
		put8bit(&buff,'.');
		putINode(&buff,SPECIAL_INODE_ROOT);
		put8bit(&buff,TYPE_DIRECTORY);
		return;
	}
}

static uint32_t dir_dataentries_size(const uint8_t *dbuff,uint32_t dsize) {
	uint8_t nleng;
	uint32_t eleng;
	const uint8_t *eptr;
	eleng=0;
	if (dbuff==NULL || dsize==0) {
		return 0;
	}
	eptr = dbuff+dsize;
	while (dbuff<eptr) {
		nleng = dbuff[0];
		dbuff+=5+nleng;
		if (nleng>255-9) {
			eleng+=6+255;
		} else {
			eleng+=6+nleng+9;
		}
	}
	return eleng;
}

static void dir_dataentries_convert(uint8_t *buff,const uint8_t *dbuff,uint32_t dsize) {
	const char *name;
	inode_t inode;
	uint8_t nleng;
	uint8_t inoleng;
	const uint8_t *eptr;
	eptr = dbuff+dsize;
	while (dbuff<eptr) {
		nleng = dbuff[0];
		if (dbuff+nleng+5<=eptr) {
			dbuff++;
			if (nleng>255-9) {
				inoleng = 255;
			} else {
				inoleng = nleng+9;
			}
			put8bit(&buff,inoleng);
			name = (const char*)dbuff;
			dbuff+=nleng;
			inode = get32bit(&dbuff);
			sprintf((char*)buff,"%08" PRIXiNode "|",inode);
			if (nleng>255-9) {
				memcpy(buff+9,name,255-9);
				buff+=255;
			} else {
				memcpy(buff+9,name,nleng);
				buff+=9+nleng;
			}
			putINode(&buff,inode);
			put8bit(&buff,TYPE_FILE);
		} else {
			safs_pretty_syslog(LOG_WARNING,"dir data malformed (trash)");
			dbuff=eptr;
		}
	}
}


static void dirbuf_meta_fill(dirbuf *b, inode_t ino) {
	int status;
	uint32_t msize, dsize = 0, dcsize;
	const uint8_t *dbuff = nullptr;

	b->p = NULL;
	b->size = 0;
	msize = dir_metaentries_size(ino);

	if (ino == SPECIAL_INODE_META_TRASH) {
		status = fs_gettrash(&dbuff, &dsize);
		if (status != SAUNAFS_STATUS_OK)
			return;
		dcsize = dir_dataentries_size(dbuff,dsize);
	} else if (ino == SPECIAL_INODE_META_RESERVED) {
		status = fs_getreserved(&dbuff, &dsize);
		if (status != SAUNAFS_STATUS_OK)
			return;
		dcsize = dir_dataentries_size(dbuff, dsize);
	} else {
		dcsize = 0;
	}

	if (msize + dcsize == 0)
		return;

	if (!(b->p = (uint8_t*)malloc(msize + dcsize))) {
		safs_pretty_syslog(LOG_EMERG, "out of memory");
		return;
	}

	if (msize > 0)
		dir_metaentries_fill(b->p, ino);

	if (dcsize > 0)
		dir_dataentries_convert(b->p + msize, dbuff, dsize);

	b->size = msize + dcsize;
}

void sfs_meta_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
	dirbuf *dirinfo;

	if (ino == SPECIAL_INODE_ROOT || ino == SPECIAL_INODE_META_TRASH ||
	    ino == SPECIAL_INODE_META_UNDEL || ino == SPECIAL_INODE_META_RESERVED) {

		if (!(dirinfo = (dirbuf*)malloc(sizeof(dirbuf)))) {
			safs_pretty_syslog(LOG_EMERG, "out of memory");
			return;
		}

		if (pthread_mutex_init(&(dirinfo->lock), NULL)) {
			free(dirinfo);
			return;
		}

		dirinfo->p = NULL;
		dirinfo->size = 0;
		dirinfo->wasread = false;
		fi->fh = (unsigned long)dirinfo;

		if (fuse_reply_open(req,fi) == -ENOENT) {
			fi->fh = 0;
			free(dirinfo->p);
			bool lock_destroy_failed = pthread_mutex_destroy(&(dirinfo->lock));
			free(dirinfo);
			if (lock_destroy_failed)
				return;
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
		sfs_meta_type_to_stat(file.inode, file.type, &statBuffer);
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

void sfs_meta_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi) {
	dirbuf *dirinfo = (dirbuf *)((unsigned long)(fi->fh));
	char buffer[READDIR_BUFFSIZE];
	char *name,c;
	const uint8_t *ptr,*eptr;
	uint8_t end;
	size_t opos,oleng;
	uint8_t nleng;
	inode_t inode;
	uint8_t type;
	struct stat stbuf;

	if (off<0) {
		fuse_reply_err(req,EINVAL);
		return;
	}
	pthread_mutex_lock(&(dirinfo->lock));
	if (!dirinfo->wasread || (dirinfo->wasread && off==0)) {
		if (dirinfo->p!=NULL) {
			free(dirinfo->p);
		}
		if (ino == SPECIAL_INODE_ROOT) {
			replyDirReadRoot(req, off, size);
			pthread_mutex_unlock(&(dirinfo->lock));
			return;
		}
		dirbuf_meta_fill(dirinfo, ino);
//              safs_pretty_syslog(LOG_WARNING,"inode: %lu , dirinfo->p: %p , dirinfo->size: %lu",(unsigned long)ino,dirinfo->p,(unsigned long)dirinfo->size);
	}
	dirinfo->wasread=true;

	if (off>=(off_t)(dirinfo->size)) {
		fuse_reply_buf(req, NULL, 0);
	} else {
		size = std::min<size_t>(size, READDIR_BUFFSIZE);
		ptr = (const uint8_t*)(dirinfo->p)+off;
		eptr = (const uint8_t*)(dirinfo->p)+dirinfo->size;
		opos = 0;
		end = 0;

		while (ptr<eptr && end==0) {
			nleng = ptr[0];
			ptr++;
			name = (char*)ptr;
			ptr+=nleng;
			off+=nleng+6;
			if (ptr+5<=eptr) {
				inode = get32bit(&ptr);
				type = get8bit(&ptr);
				sfs_meta_type_to_stat(inode,type,&stbuf);
				c = name[nleng];
				name[nleng]=0;
				oleng = fuse_add_direntry(req, buffer + opos, size - opos, name, &stbuf, off);
				name[nleng] = c;
				if (opos+oleng>size) {
					end=1;
				} else {
					opos+=oleng;
				}
			}
		}
		fuse_reply_buf(req,buffer,opos);
	}
	pthread_mutex_unlock(&(dirinfo->lock));
}

void sfs_meta_releasedir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
	(void)ino;
	dirbuf *dirinfo = (dirbuf *)((unsigned long)(fi->fh));
	pthread_mutex_lock(&(dirinfo->lock));
	pthread_mutex_unlock(&(dirinfo->lock));
	pthread_mutex_destroy(&(dirinfo->lock));
	free(dirinfo->p);
	free(dirinfo);
	fi->fh = 0;
	fuse_reply_err(req,0);
}

void sfs_meta_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
	pathbuf *pathinfo;
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

	status = fs_gettrashpath(ino,&path);
	status = saunafs_error_conv(status);

	if (status) {
		fuse_reply_err(req, status);
	} else {
		if (!(pathinfo = (pathbuf*)malloc(sizeof(pathbuf)))) {
			safs_pretty_syslog(LOG_EMERG, "out of memory");
			return;
		}

		if (pthread_mutex_init(&(pathinfo->lock), NULL)) {
			free(pathinfo);
			return;
		}

		pathinfo->changed = 0;
		pathinfo->size = strlen((char*)path) + 1;
		if (!(pathinfo->p = (char*)malloc(pathinfo->size))) {
			safs_pretty_syslog(LOG_EMERG, "out of memory");
			pthread_mutex_destroy(&(pathinfo->lock));
			free(pathinfo);
			return;
		}
		memcpy(pathinfo->p, path, pathinfo->size - 1);
		pathinfo->p[pathinfo->size - 1] = '\n';
		fi->direct_io = 1;
		fi->fh = (unsigned long)pathinfo;

		if (fuse_reply_open(req, fi) == -ENOENT) {
			fi->fh = 0;
			pthread_mutex_destroy(&(pathinfo->lock));
			free(pathinfo->p);
			free(pathinfo);
		}
	}
}

void sfs_meta_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
	if (ino==SPECIAL_INODE_MASTERINFO) {
		fuse_reply_err(req,0);
		return;
	}
	pathbuf *pathinfo = (pathbuf *)((unsigned long)(fi->fh));
	pthread_mutex_lock(&(pathinfo->lock));
	if (pathinfo->changed) {
		if (pathinfo->p[pathinfo->size-1]=='\n') {
			pathinfo->p[pathinfo->size-1]=0;
		} else {
			pathinfo->p = (char*) realloc(pathinfo->p,pathinfo->size+1);
			pathinfo->p[pathinfo->size]=0;
		}
		fs_settrashpath(ino,(uint8_t*)pathinfo->p);
	}
	pthread_mutex_unlock(&(pathinfo->lock));
	pthread_mutex_destroy(&(pathinfo->lock));
	free(pathinfo->p);
	free(pathinfo);
	fi->fh = 0;
	fuse_reply_err(req,0);
}

void sfs_meta_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi) {
	pathbuf *pathinfo = (pathbuf *)((unsigned long)(fi->fh));
	if (ino==SPECIAL_INODE_MASTERINFO) {
		uint8_t masterinfo[14];
		fs_getmasterlocation(masterinfo);
		masterproxy_getlocation(masterinfo);
#ifdef MASTERINFO_WITH_VERSION
		if (off>=14) {
			fuse_reply_buf(req,NULL,0);
		} else if (off+size>14) {
			fuse_reply_buf(req,(char*)(masterinfo+off),14-off);
#else
		if (off>=10) {
			fuse_reply_buf(req,NULL,0);
		} else if (off+size>10) {
			fuse_reply_buf(req,(char*)(masterinfo+off),10-off);
#endif
		} else {
			fuse_reply_buf(req,(char*)(masterinfo+off),size);
		}
		return;
	}
	if (pathinfo==NULL) {
		fuse_reply_err(req,EBADF);
		return;
	}
	pthread_mutex_lock(&(pathinfo->lock));
	if (off<0) {
		pthread_mutex_unlock(&(pathinfo->lock));
		fuse_reply_err(req,EINVAL);
		return;
	}
	if ((size_t)off>pathinfo->size) {
		fuse_reply_buf(req, NULL, 0);
	} else if (off + size > pathinfo->size) {
		fuse_reply_buf(req, (pathinfo->p)+off,(pathinfo->size)-off);
	} else {
		fuse_reply_buf(req, (pathinfo->p)+off,size);
	}
	pthread_mutex_unlock(&(pathinfo->lock));
}

void sfs_meta_write(fuse_req_t req, fuse_ino_t ino, const char *buf, size_t size, off_t off, struct fuse_file_info *fi) {
	pathbuf *pathinfo = (pathbuf *)((unsigned long)(fi->fh));
	if (ino==SPECIAL_INODE_MASTERINFO) {
		fuse_reply_err(req,EACCES);
		return;
	}
	if (pathinfo==NULL) {
		fuse_reply_err(req,EBADF);
		return;
	}
	if (off + size > PATH_SIZE_LIMIT) {
		fuse_reply_err(req,EINVAL);
		return;
	}
	pthread_mutex_lock(&(pathinfo->lock));
	if (pathinfo->changed==0) {
		pathinfo->size = 0;
	}
	if (off+size > pathinfo->size) {
		size_t s = pathinfo->size;
		pathinfo->p = (char*) realloc(pathinfo->p,off+size);
		pathinfo->size = off+size;
		memset(pathinfo->p+s,0,off+size-s);
	}
	memcpy((pathinfo->p)+off,buf,size);
	pathinfo->changed = 1;
	pthread_mutex_unlock(&(pathinfo->lock));
	fuse_reply_write(req,size);
}

void sfs_meta_init(int debug_mode_in,double entry_cache_timeout_in,double attr_cache_timeout_in) {
	debug_mode = debug_mode_in;
	entry_cache_timeout = entry_cache_timeout_in;
	attr_cache_timeout = attr_cache_timeout_in;
	if (debug_mode) {
		fprintf(stderr,"cache parameters: entry_cache_timeout=%.2f attr_cache_timeout=%.2f\n",entry_cache_timeout,attr_cache_timeout);
	}
}

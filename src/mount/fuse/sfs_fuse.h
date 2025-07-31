/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2018 Skytechnology sp. z o.o.
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

#pragma once

#include "common/platform.h"

#include <fuse.h>
#include <fuse_lowlevel.h>

#include "protocol/SFSCommunication.h"

void sfs_statfs(fuse_req_t req, fuse_ino_t ino);
void sfs_access(fuse_req_t req, fuse_ino_t ino, int mask);
void sfs_lookup(fuse_req_t req, fuse_ino_t parent, const char *name);
void sfs_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi);
void sfs_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *stbuf, int to_set, struct fuse_file_info *fi);
void sfs_mknod(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode, dev_t rdev);
void sfs_unlink(fuse_req_t req, fuse_ino_t parent, const char *name);
void sfs_mkdir(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode);
void sfs_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name);
void sfs_symlink(fuse_req_t req, const char *path, fuse_ino_t parent, const char *name);
void sfs_readlink(fuse_req_t req, fuse_ino_t ino);
void sfs_rename(fuse_req_t req, fuse_ino_t parent, const char *name, fuse_ino_t newparent, const char *newname, unsigned int flags);
void sfs_link(fuse_req_t req, fuse_ino_t ino, fuse_ino_t newparent, const char *newname);
void sfs_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi);
void sfs_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi);
void sfs_releasedir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi);
void sfs_create(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode, struct fuse_file_info *fi);
void sfs_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi);
void sfs_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi);
void sfs_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi);
void sfs_write(fuse_req_t req, fuse_ino_t ino, const char *buf, size_t size, off_t off, struct fuse_file_info *fi);
void sfs_flush(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi);
void sfs_fsync(fuse_req_t req, fuse_ino_t ino, int datasync, struct fuse_file_info *fi);
#if defined(__APPLE__)
void sfs_setxattr (fuse_req_t req, fuse_ino_t ino, const char *name, const char *value, size_t size, int flags, uint32_t position);
void sfs_getxattr (fuse_req_t req, fuse_ino_t ino, const char *name, size_t size, uint32_t position);
#else
void sfs_setxattr (fuse_req_t req, fuse_ino_t ino, const char *name, const char *value, size_t size, int flags);
void sfs_getxattr (fuse_req_t req, fuse_ino_t ino, const char *name, size_t size);
#endif /* __APPLE__ */
void sfs_listxattr (fuse_req_t req, fuse_ino_t ino, size_t size);
void sfs_removexattr (fuse_req_t req, fuse_ino_t ino, const char *name);
void safs_getlk(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi, struct flock *lock);
void safs_setlk(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi, struct flock *lock, int sleep) ;
void safs_flock(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi, int op);

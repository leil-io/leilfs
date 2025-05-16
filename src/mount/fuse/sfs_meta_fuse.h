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

#include <fuse_lowlevel.h>

void sfs_meta_statfs(fuse_req_t req, fuse_ino_t ino);
void sfs_meta_lookup(fuse_req_t req, fuse_ino_t parent, const char *name);
void sfs_meta_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi);
void sfs_meta_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *stbuf, int to_set, struct fuse_file_info *fi);
void sfs_meta_unlink(fuse_req_t req, fuse_ino_t parent, const char *name);
void sfs_meta_rename(fuse_req_t req, fuse_ino_t parent, const char *name, fuse_ino_t newparent, const char *newname, unsigned int flags);
void sfs_meta_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi);
void sfs_meta_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi);
void sfs_meta_releasedir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi);
void sfs_meta_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi);
void sfs_meta_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi);
void sfs_meta_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi);
void sfs_meta_write(fuse_req_t req, fuse_ino_t ino, const char *buf, size_t size, off_t off, struct fuse_file_info *fi);
void sfs_meta_init(double entry_cache_timeout_in, double attr_cache_timeout_in);

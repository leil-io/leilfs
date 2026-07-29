/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2017 Skytechnology sp. z o.o.
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

#include "master/restore.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/type_defs.h"
#include "errors/saunafs_error_codes.h"
#include "master/filesystem.h"
#include "master/filesystem_node_types.h"
#include "master/filesystem_operations.h"
#include "master/filesystem_operations_interface.h"
#include "master/filesystem_snapshot.h"
#include "protocol/SFSCommunication.h"
#include "slogger/slogger.h"

#define EAT(clptr,fn,vno,c) { \
	if (*(clptr)!=(c)) { \
		safs_pretty_syslog(LOG_ERR, "%s:%" PRIu64 ": '%c' expected", (fn), (vno), (c)); \
		return -1; \
	} \
	(clptr)++; \
}

#define GETNAME(name,clptr,fn,vno,c) { \
	uint32_t _tmp_i; \
	char _tmp_c,_tmp_h1,_tmp_h2; \
	memset((void*)(name),0,256); \
	_tmp_i = 0; \
	while ((_tmp_c=*((clptr)++))!=c && _tmp_i<255) { \
		if (_tmp_c=='%') { \
			_tmp_h1 = *((clptr)++); \
			_tmp_h2 = *((clptr)++); \
			if (_tmp_h1>='0' && _tmp_h1<='9') { \
				_tmp_h1-='0'; \
			} else if (_tmp_h1>='A' && _tmp_h1<='F') { \
				_tmp_h1-=('A'-10); \
			} else { \
				safs_pretty_syslog(LOG_ERR, "%s:%" PRIu64 ": hex expected", (fn), (vno)); \
				return -1; \
			} \
			if (_tmp_h2>='0' && _tmp_h2<='9') { \
				_tmp_h2-='0'; \
			} else if (_tmp_h2>='A' && _tmp_h2<='F') { \
				_tmp_h2-=('A'-10); \
			} else { \
				safs_pretty_syslog(LOG_ERR, "%s:%" PRIu64 ": hex expected", (fn), (vno)); \
				return -1; \
			} \
			_tmp_c = _tmp_h1*16+_tmp_h2; \
		} \
		name[_tmp_i++] = _tmp_c; \
	} \
	(clptr)--; \
	name[_tmp_i]=0; \
}

#define GETPATH(path,size,clptr,fn,vno,c) { \
	uint32_t _tmp_i; \
	char _tmp_c,_tmp_h1,_tmp_h2; \
	_tmp_i = 0; \
	while ((_tmp_c=*((clptr)++))!=c) { \
		if (_tmp_c=='%') { \
			_tmp_h1 = *((clptr)++); \
			_tmp_h2 = *((clptr)++); \
			if (_tmp_h1>='0' && _tmp_h1<='9') { \
				_tmp_h1-='0'; \
			} else if (_tmp_h1>='A' && _tmp_h1<='F') { \
				_tmp_h1-=('A'-10); \
			} else { \
				safs_pretty_syslog(LOG_ERR, "%s:%" PRIu64 ": hex expected", (fn), (vno)); \
				return -1; \
			} \
			if (_tmp_h2>='0' && _tmp_h2<='9') { \
				_tmp_h2-='0'; \
			} else if (_tmp_h2>='A' && _tmp_h2<='F') { \
				_tmp_h2-=('A'-10); \
			} else { \
				safs_pretty_syslog(LOG_ERR, "%s:%" PRIu64 ": hex expected", (fn), (vno)); \
				return -1; \
			} \
			_tmp_c = _tmp_h1*16+_tmp_h2; \
		} \
		if ((_tmp_i)>=(size)) { \
			(size) = _tmp_i+1000; \
			if ((path)==NULL) { \
				(path) = (uint8_t*)malloc(size); \
			} else { \
				uint8_t *_tmp_path = (path); \
				(path) = (uint8_t*)realloc((path),(size)); \
				if ((path)==NULL) { \
					free(_tmp_path); \
				} \
			} \
			if ((path)==NULL) { \
				safs_pretty_syslog(LOG_ERR, "out of memory"); \
				exit(1); \
			} \
		} \
		(path)[_tmp_i++]=_tmp_c; \
	} \
	if ((_tmp_i)>=(size)) { \
		(size) = _tmp_i+1000; \
		if ((path)==NULL) { \
			(path) = (uint8_t*)malloc(size); \
		} else { \
			uint8_t *_tmp_path = (path); \
			(path) = (uint8_t*)realloc((path),(size)); \
			if ((path)==NULL) { \
				free(_tmp_path); \
			} \
		} \
		if ((path)==NULL) { \
			safs_pretty_syslog(LOG_ERR, "out of memory"); \
			exit(1); \
		} \
	} \
	(clptr)--; \
	(path)[_tmp_i]=0; \
}

#define GETDATA(buff,leng,size,clptr,fn,vno,c) { \
	char _tmp_c,_tmp_h1,_tmp_h2; \
	(leng) = 0; \
	while ((_tmp_c=*((clptr)++))!=c) { \
		if (_tmp_c=='%') { \
			_tmp_h1 = *((clptr)++); \
			_tmp_h2 = *((clptr)++); \
			if (_tmp_h1>='0' && _tmp_h1<='9') { \
				_tmp_h1-='0'; \
			} else if (_tmp_h1>='A' && _tmp_h1<='F') { \
				_tmp_h1-=('A'-10); \
			} else { \
				safs_pretty_syslog(LOG_ERR, "%s:%" PRIu64 ": hex expected", (fn), (vno)); \
				return -1; \
			} \
			if (_tmp_h2>='0' && _tmp_h2<='9') { \
				_tmp_h2-='0'; \
			} else if (_tmp_h2>='A' && _tmp_h2<='F') { \
				_tmp_h2-=('A'-10); \
			} else { \
				safs_pretty_syslog(LOG_ERR, "%s:%" PRIu64 ": hex expected", (fn), (vno)); \
				return -1; \
			} \
			_tmp_c = _tmp_h1*16+_tmp_h2; \
		} \
		if ((leng)>=(size)) { \
			(size) = (leng)+1000; \
			if ((buff)==NULL) { \
				(buff) = (uint8_t*)malloc(size); \
			} else { \
				uint8_t *_tmp_buff = (buff); \
				(buff) = (uint8_t*)realloc((buff),(size)); \
				if ((buff)==NULL) { \
					free(_tmp_buff); \
				} \
			} \
			if ((buff)==NULL) { \
				safs_pretty_syslog(LOG_ERR, "out of memory"); \
				exit(1); \
			} \
		} \
		(buff)[(leng)++]=_tmp_c; \
	} \
	(clptr)--; \
}

#define GETCHAR(data,clptr) { \
	if (*(clptr)) { \
		(data) = *((clptr)++); \
	} \
}

#define GETU32(data,clptr) do { char* end_ = NULL; (data)=strtoul(clptr,&end_,10); clptr = end_; } while (0)
#define GETU64(data,clptr) do { char* end_ = NULL; (data)=strtoull(clptr,&end_,10); clptr = end_; } while (0)

#ifdef SAUNAFS_USE_INODE64
#define GETINODE(data,clptr) GETU64(data,clptr)
#else
#define GETINODE(data,clptr) GETU32(data,clptr)
#endif

int do_access(const char *filename, uint64_t lv, uint32_t ts, const char *ptr) {
	inode_t inode;
	EAT(ptr,filename,lv,'(');
	GETINODE(inode,ptr);
	EAT(ptr,filename,lv,')');
	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	int status = gFSOperations->applyAccess(fsOpContext, ts, inode);

	if (status == SAUNAFS_STATUS_OK && fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.commitTransaction()) {
			safs::log_err("{}: transaction failed to commit: inode {}", __func__, inode);
			status = SAUNAFS_ERROR_IO;
		}
	}

	return status;
}

int do_append(const char *filename, uint64_t lv, uint32_t ts, const char *ptr) {
	inode_t inode;
	inode_t inode_src;
	EAT(ptr,filename,lv,'(');
	GETINODE(inode,ptr);
	EAT(ptr,filename,lv,',');
	GETINODE(inode_src,ptr);
	EAT(ptr,filename,lv,')');

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	int status = gFSOperations->append(FsContext::getForRestore(ts), fsOpContext, inode, inode_src);

	if (status == SAUNAFS_STATUS_OK && fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.commitTransaction()) {
			safs::log_err("{}: transaction failed to commit: inode {}, source inode {}", __func__,
			              inode, inode_src);
			status = SAUNAFS_ERROR_IO;
		}
	}

	return status;
}

int do_acquire(const char *filename, uint64_t lv, uint32_t ts, const char *ptr) {
	inode_t inode;
	uint32_t cuid;
	EAT(ptr,filename,lv,'(');
	GETINODE(inode,ptr);
	EAT(ptr,filename,lv,',');
	GETU32(cuid,ptr);
	EAT(ptr,filename,lv,')');

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	int status = gFSOperations->acquire(FsContext::getForRestore(ts), fsOpContext, inode, cuid);

	if (status == SAUNAFS_STATUS_OK && fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.commitTransaction()) {
			safs::log_err("{}: transaction failed to commit: inode {}, session id {}", __func__,
			              inode, cuid);
			status = SAUNAFS_ERROR_IO;
		}
	}

	return status;
}

int do_attr(const char *filename, uint64_t lv, uint32_t ts, const char *ptr) {
	inode_t inode;
	uint32_t mode,uid,gid,atime,mtime;
	EAT(ptr,filename,lv,'(');
	GETINODE(inode,ptr);
	EAT(ptr,filename,lv,',');
	GETU32(mode,ptr);
	EAT(ptr,filename,lv,',');
	GETU32(uid,ptr);
	EAT(ptr,filename,lv,',');
	GETU32(gid,ptr);
	EAT(ptr,filename,lv,',');
	GETU32(atime,ptr);
	EAT(ptr,filename,lv,',');
	GETU32(mtime,ptr);
	EAT(ptr,filename,lv,')');

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	int status = gFSOperations->applyAttr(fsOpContext, ts, inode, mode, uid, gid, atime, mtime);

	if (status == SAUNAFS_STATUS_OK && fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.commitTransaction()) {
			safs::log_err(
			    "{}: transaction failed to commit: inode {}, mode {}, uid {}, gid {}, atime {}, mtime {}",
			    __func__, inode, mode, uid, gid, atime, mtime);
			status = SAUNAFS_ERROR_IO;
		}
	}

	return status;
}

int do_checksum(const char *filename, uint64_t lv, uint32_t, const char *ptr) {
	uint8_t version[256];
	uint64_t checksum;
	EAT(ptr,filename,lv,'(');
	GETNAME(version,ptr,filename,lv,')');
	EAT(ptr,filename,lv,')');
	EAT(ptr,filename,lv,':');
	GETU64(checksum,ptr);
	return gFSOperations->applyChecksum((char *)&version, checksum);
}

int do_create(const char *filename, uint64_t lv, uint32_t ts, const char *ptr) {
	inode_t parent;
	inode_t inode;
	uint32_t mode,uid,gid,rdev;
	uint8_t type,name[256];
	EAT(ptr,filename,lv,'(');
	GETINODE(parent,ptr);
	EAT(ptr,filename,lv,',');
	GETNAME(name,ptr,filename,lv,',');
	EAT(ptr,filename,lv,',');
	type = *ptr;
	ptr++;
	EAT(ptr,filename,lv,',');
	GETU32(mode,ptr);
	EAT(ptr,filename,lv,',');
	GETU32(uid,ptr);
	EAT(ptr,filename,lv,',');
	GETU32(gid,ptr);
	EAT(ptr,filename,lv,',');
	GETU32(rdev,ptr);
	EAT(ptr,filename,lv,')');
	EAT(ptr,filename,lv,':');
	GETINODE(inode,ptr);
	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	int status = gFSOperations->applyCreate(fsOpContext, ts, parent, HString((const char *)name),
	                                        static_cast<FSNodeType>(type), mode, uid, gid, rdev, inode);

	if (status == SAUNAFS_STATUS_OK && fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.commitTransaction()) {
			safs::log_err(
			    "{}: transaction failed to commit: parent {}, type {}, inode {}",
			    __func__, parent, type, inode);
			status = SAUNAFS_ERROR_IO;
		}
	}

	return status;
}

int do_session(const char *filename, uint64_t lv, uint32_t ts, const char *ptr) {
	uint32_t cuid;
	(void)ts;
	EAT(ptr,filename,lv,'(');
	EAT(ptr,filename,lv,')');
	EAT(ptr,filename,lv,':');
	GETU32(cuid,ptr);
	return gFSOperations->applySession(cuid);
}

int do_incversion(const char *filename, uint64_t lv, uint32_t ts, const char *ptr) {
	uint64_t chunkid;
	(void)ts;
	EAT(ptr,filename,lv,'(');
	GETU64(chunkid,ptr);
	EAT(ptr,filename,lv,')');

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	int status = gFSOperations->applyIncreaseChunkVersion(fsOpContext, chunkid);

	if (status == SAUNAFS_STATUS_OK && fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.commitTransaction()) {
			safs::log_err("{}: transaction failed to commit: chunk {}", __func__, chunkid);
			status = SAUNAFS_ERROR_IO;
		}
	}

	return status;
}

int do_link(const char *filename, uint64_t lv, uint32_t ts, const char *ptr) {
	inode_t inode;
	inode_t parent;
	uint8_t name[256];
	EAT(ptr,filename,lv,'(');
	GETINODE(inode,ptr);
	EAT(ptr,filename,lv,',');
	GETINODE(parent,ptr);
	EAT(ptr,filename,lv,',');
	GETNAME(name,ptr,filename,lv,')');
	EAT(ptr,filename,lv,')');

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	int status = gFSOperations->link(FsContext::getForRestore(ts), fsOpContext, inode, parent,
	                                 HString((const char *)name), nullptr, nullptr);

	if (status == SAUNAFS_STATUS_OK && fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.commitTransaction()) {
			safs::log_err("{}: transaction failed to commit: inode {}, parent inode {}, name {}",
			              __func__, inode, parent,
			              std::string(reinterpret_cast<const char *>(name)));
			status = SAUNAFS_ERROR_IO;
		}
	}

	return status;
}

int do_length(const char *filename, uint64_t lv, uint32_t ts, const char *ptr) {
	inode_t inode;
	uint64_t length;
	uint32_t eraseFurtherChunks;
	EAT(ptr, filename, lv, '(');
	GETINODE(inode, ptr);
	EAT(ptr, filename, lv, ',');
	GETU64(length, ptr);
	if (*ptr == ')') {
		// Old metadata version was always deleting further chunks
		eraseFurtherChunks = 1;
	} else {
		// New metadata version logs whether further chunks must be deleted
		EAT(ptr, filename, lv, ',');
		GETU32(eraseFurtherChunks, ptr);
	}
	EAT(ptr, filename, lv, ')');

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	int status =
	    gFSOperations->applyLength(fsOpContext, ts, inode, length, eraseFurtherChunks != 0);

	if (status == SAUNAFS_STATUS_OK && fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.commitTransaction()) {
			safs::log_err(
			    "{}: transaction failed to commit: inode {}, length {}, eraseFurtherChunks {}",
			    __func__, inode, length, eraseFurtherChunks);
			status = SAUNAFS_ERROR_IO;
		}
	}

	return status;
}

int do_move(const char* filename, uint64_t lv, uint32_t ts, const char* ptr) {
	inode_t inode;
	inode_t parent_src;
	inode_t parent_dst;
	uint8_t name_src[256],name_dst[256];
	EAT(ptr,filename,lv,'(');
	GETINODE(parent_src,ptr);
	EAT(ptr,filename,lv,',');
	GETNAME(name_src,ptr,filename,lv,',');
	EAT(ptr,filename,lv,',');
	GETINODE(parent_dst,ptr);
	EAT(ptr,filename,lv,',');
	GETNAME(name_dst,ptr,filename,lv,')');
	EAT(ptr,filename,lv,')');
	EAT(ptr,filename,lv,':');
	GETINODE(inode,ptr);

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	int status = gFSOperations->rename(FsContext::getForRestore(ts), fsOpContext, parent_src,
	                                   HString((const char *)name_src), parent_dst,
	                                   HString((const char *)name_dst), &inode, nullptr);

	if (status == SAUNAFS_STATUS_OK && fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.commitTransaction()) {
			safs::log_err(
			    "{}: transaction failed to commit: src inode {}, src name {}, dst inode {}, dst name {}",
			    __func__, parent_src, (char *)name_src, parent_dst, (char *)name_dst);
			status = SAUNAFS_ERROR_IO;
		}
	}

	return status;
}

int do_lock_op(const char *filename, uint64_t lv, uint32_t ts, const char *ptr) {
	uint32_t lock_type, sessionid;
	inode_t inode;
	uint64_t start, end;
	uint64_t owner;
	uint32_t op;
	const bool nonblocking = false;
	std::vector<FileLocks::Owner> dummy_applied;
	EAT(ptr, filename, lv, '(');
	GETU32(lock_type, ptr);
	EAT(ptr, filename, lv, ',');
	GETINODE(inode, ptr);
	EAT(ptr, filename, lv, ',');
	GETU64(start, ptr);
	EAT(ptr ,filename, lv, ',');
	GETU64(end, ptr);
	EAT(ptr, filename, lv, ',');
	GETU64(owner, ptr);
	EAT(ptr, filename, lv, ',');
	GETU32(sessionid, ptr);
	EAT(ptr, filename, lv, ',');
	GETU32(op, ptr);
	EAT(ptr,filename,lv,')');

	int status = SAUNAFS_STATUS_OK;

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	switch (static_cast<safs_locks::Type>(lock_type)) {
	case safs_locks::Type::kFlock:
		status =
		    gFSOperations->flockOperation(FsContext::getForRestore(ts), fsOpContext, inode, owner,
		                                  sessionid, 0, 0, op, nonblocking, dummy_applied);
		break;
	case safs_locks::Type::kPosix:
		status = gFSOperations->posixLockOperation(FsContext::getForRestore(ts), fsOpContext, inode,
		                                           start, end, owner, sessionid, 0, 0, op,
		                                           nonblocking, dummy_applied);
		break;
	default:
		safs_pretty_syslog(LOG_ERR, "Invalid lock type passed to restore: %u", lock_type);
		return SAUNAFS_ERROR_EINVAL;
	}

	if ((status == SAUNAFS_STATUS_OK || status == SAUNAFS_ERROR_WAITING) &&
	    fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.commitTransaction()) {
			safs::log_err("{}: transaction failed to commit: inode {}", __func__, inode);
			status = SAUNAFS_ERROR_IO;
		}
	}

	if (status==SAUNAFS_ERROR_WAITING) {
		return SAUNAFS_STATUS_OK;
	}
	return status;
}

int do_remove_pending_op(const char *filename, uint64_t lv, uint32_t ts, const char *ptr) {
	uint32_t lock_type;
	uint64_t ownerid;
	uint32_t sessionid;
	inode_t inode;
	uint64_t reqid;

	EAT(ptr, filename, lv, '(');
	GETU32(lock_type, ptr);
	EAT(ptr, filename, lv, ',');
	GETU64(ownerid, ptr);
	EAT(ptr, filename, lv, ',');
	GETU32(sessionid, ptr);
	EAT(ptr ,filename, lv, ',');
	GETINODE(inode, ptr);
	EAT(ptr, filename, lv, ',');
	GETU64(reqid, ptr);
	EAT(ptr,filename,lv,')');

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	int status = gFSOperations->locksRemovePending(FsContext::getForRestore(ts), fsOpContext,
	                                               lock_type, ownerid, sessionid, inode, reqid);

	if (status == SAUNAFS_STATUS_OK && fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.commitTransaction()) {
			safs::log_err("{}: transaction failed to commit: inode {}", __func__, inode);
			status = SAUNAFS_ERROR_IO;
		}
	}

	return status;
}

int do_lock_clear_session(const char *filename, uint64_t lv, uint32_t ts, const char *ptr) {
	uint32_t lock_type, sessionid;
	inode_t inode;
	std::vector<FileLocks::Owner> applied;

	EAT(ptr, filename, lv, '(');
	GETU32(lock_type, ptr);
	EAT(ptr, filename, lv, ',');
	GETINODE(inode, ptr);
	EAT(ptr, filename, lv, ',');
	GETU32(sessionid, ptr);
	EAT(ptr, filename, lv, ')');

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	int status = gFSOperations->locksClearSession(FsContext::getForRestore(ts), fsOpContext,
	                                              lock_type, inode, sessionid, applied);

	if (status == SAUNAFS_STATUS_OK && fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.commitTransaction()) {
			safs::log_err("{}: transaction failed to commit: inode {}", __func__, inode);
			status = SAUNAFS_ERROR_IO;
		}
	}

	return status;
}

int do_lock_unlock_inode(const char *filename, uint64_t lv, uint32_t ts, const char *ptr) {
	uint32_t lock_type;
	inode_t inode;
	std::vector<FileLocks::Owner> applied;

	EAT(ptr, filename, lv, '(');
	GETU32(lock_type, ptr);
	EAT(ptr, filename, lv, ',');
	GETINODE(inode, ptr);
	EAT(ptr, filename, lv, ')');

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	int status = gFSOperations->locksUnlockInode(FsContext::getForRestore(ts), fsOpContext,
	                                             lock_type, inode, applied);

	if (status == SAUNAFS_STATUS_OK && fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.commitTransaction()) {
			safs::log_err("{}: transaction failed to commit: inode {}", __func__, inode);
			status = SAUNAFS_ERROR_IO;
		}
	}

	return status;
}

int do_purge(const char* filename, uint64_t lv, uint32_t ts, const char* ptr) {
	inode_t inode;
	EAT(ptr,filename,lv,'(');
	GETINODE(inode,ptr);
	EAT(ptr,filename,lv,')');

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	int status = gFSOperations->purge(FsContext::getForRestore(ts), fsOpContext, inode);

	if (status == SAUNAFS_STATUS_OK && fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.commitTransaction()) {
			safs::log_err("{}: transaction failed to commit: inode {}", __func__, inode);
			status = SAUNAFS_ERROR_IO;
		}
	}

	return status;
}

int do_release(const char* filename, uint64_t lv, uint32_t ts, const char* ptr) {
	inode_t inode;
	uint32_t cuid;
	EAT(ptr,filename,lv,'(');
	GETINODE(inode,ptr);
	EAT(ptr,filename,lv,',');
	GETU32(cuid,ptr);
	EAT(ptr,filename,lv,')');

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	int status = gFSOperations->release(FsContext::getForRestore(ts), fsOpContext, inode, cuid);

	if (status == SAUNAFS_STATUS_OK && fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.commitTransaction()) {
			safs::log_err("{}: transaction failed to commit: inode {}, cuid {}", __func__, inode,
			              cuid);
			status = SAUNAFS_ERROR_IO;
		}
	}

	return status;
}

int do_repair(const char* filename, uint64_t lv, uint32_t ts, const char* ptr) {
	inode_t inode;
	uint32_t indx;
	uint32_t version;
	EAT(ptr,filename,lv,'(');
	GETINODE(inode,ptr);
	EAT(ptr,filename,lv,',');
	GETU32(indx,ptr);
	EAT(ptr,filename,lv,')');
	EAT(ptr,filename,lv,':');
	GETU32(version,ptr);

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	int status = gFSOperations->applyRepair(fsOpContext, ts, inode, indx, version);

	if (status == SAUNAFS_STATUS_OK && fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.commitTransaction()) {
			safs::log_err("{}: transaction failed to commit: inode {}, chunk index {}, version {}",
			              __func__, inode, indx, version);
			status = SAUNAFS_ERROR_IO;
		}
	}

	return status;
}

int do_seteattr(const char* filename, uint64_t lv, uint32_t ts, const char* ptr) {
	inode_t inode;
	inode_t ci;
	inode_t nci;
	inode_t npi;
	uint32_t uid;
	uint8_t eattr,smode;
	EAT(ptr,filename,lv,'(');
	GETINODE(inode,ptr);
	EAT(ptr,filename,lv,',');
	GETU32(uid,ptr);
	EAT(ptr,filename,lv,',');
	GETU32(eattr,ptr);
	EAT(ptr,filename,lv,',');
	GETU32(smode,ptr);
	EAT(ptr,filename,lv,')');
	EAT(ptr,filename,lv,':');
	GETINODE(ci,ptr);
	EAT(ptr,filename,lv,',');
	GETINODE(nci,ptr);
	EAT(ptr,filename,lv,',');
	GETINODE(npi,ptr);

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	int status = gFSOperations->setExtraAttr(FsContext::getForRestoreWithUidGid(ts, uid, 0),
	                                         fsOpContext, inode, eattr, smode, &ci, &nci, &npi);

	if (status == SAUNAFS_STATUS_OK && fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.commitTransaction()) {
			safs::log_err("{}: transaction failed to commit: inode {}, uid {}, eattr {}, smode {}",
			              __func__, inode, uid, static_cast<uint32_t>(eattr),
			              static_cast<uint32_t>(smode));
			status = SAUNAFS_ERROR_IO;
		}
	}

	return status;
}

int do_setgoal(const char *filename, uint64_t lv, uint32_t ts, const char *ptr) {
	inode_t inode;
	inode_t ci;
	uint32_t uid;
	uint8_t goal, smode;
	EAT(ptr, filename, lv, '(');
	GETINODE(inode, ptr);
	EAT(ptr, filename, lv, ',');
	GETU32(uid, ptr);
	EAT(ptr, filename, lv, ',');
	GETU32(goal, ptr);
	EAT(ptr, filename, lv, ',');
	GETU32(smode, ptr);
	EAT(ptr, filename, lv, ')');
	if (*(ptr) == ':') {
		EAT(ptr, filename, lv, ':');
		GETINODE(ci, ptr);
		return gFSOperations->applySetGoal(FsContext::getForRestoreWithUidGid(ts, uid, 0), inode,
		                                   goal, smode, ci);
	} else {
		return gFSOperations->applySetGoal(FsContext::getForRestoreWithUidGid(ts, uid, 0), inode,
		                                   goal, smode, SetGoalTask::kChanged);
	}
}

int do_setpath(const char* filename, uint64_t lv, uint32_t ts, const char* ptr) {
	inode_t inode;
	static uint8_t *path = NULL;
	static uint32_t pathsize = 0;
	EAT(ptr,filename,lv,'(');
	GETINODE(inode,ptr);
	EAT(ptr,filename,lv,',');
	GETPATH(path,pathsize,ptr,filename,lv,')');
	EAT(ptr,filename,lv,')');
	return gFSOperations->setTrashPath(FsContext::getForRestore(ts), inode,
	                                   std::string((const char *)path));
}

int do_settrashtime(const char *filename, uint64_t lv, uint32_t ts, const char *ptr) {
	inode_t inode;
	inode_t ci;
	uint32_t uid;
	uint32_t trashtime;
	uint8_t smode;
	EAT(ptr, filename, lv, '(');
	GETINODE(inode, ptr);
	EAT(ptr, filename, lv, ',');
	GETU32(uid, ptr);
	EAT(ptr, filename, lv, ',');
	GETU32(trashtime, ptr);
	EAT(ptr, filename, lv, ',');
	GETU32(smode, ptr);
	EAT(ptr, filename, lv, ')');
	if ((*ptr) == ':') {
		EAT(ptr, filename, lv, ':');
		GETINODE(ci, ptr);
		return gFSOperations->applySetTrashTime(FsContext::getForRestoreWithUidGid(ts, uid, 0),
		                                        inode, trashtime, smode, ci);
	} else {
		return gFSOperations->applySetTrashTime(FsContext::getForRestoreWithUidGid(ts, uid, 0),
		                                        inode, trashtime, smode,
		                                        SetTrashtimeTask::kChanged);
	}
}

int do_setxattr(const char* filename, uint64_t lv, uint32_t ts, const char* ptr) {
	inode_t inode;
	uint32_t valueleng,mode;
	uint8_t name[256];
	static uint8_t *value = NULL;
	static uint32_t valuesize = 0;
	EAT(ptr,filename,lv,'(');
	GETINODE(inode,ptr);
	EAT(ptr,filename,lv,',');
	GETNAME(name,ptr,filename,lv,',');
	EAT(ptr,filename,lv,',');
	GETDATA(value,valueleng,valuesize,ptr,filename,lv,',');
	EAT(ptr,filename,lv,',');
	GETU32(mode,ptr);
	EAT(ptr,filename,lv,')');

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	int status = gFSOperations->applySetXAttr(fsOpContext, ts, inode, strlen((char *)name), name,
	                                          valueleng, value, mode);

	if (status == SAUNAFS_STATUS_OK && fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.commitTransaction()) {
			safs::log_err("{}: transaction failed to commit: inode {}", __func__, inode);
			status = SAUNAFS_ERROR_IO;
		}
	}

	return status;
}

int do_deleteacl(const char *filename, uint64_t lv, uint32_t ts, const char *ptr) {
	inode_t inode;
	char aclTypeRaw = '\0';

	EAT(ptr, filename, lv, '(');
	GETINODE(inode, ptr);
	EAT(ptr, filename, lv, ',');
	GETCHAR(aclTypeRaw, ptr);
	EAT(ptr, filename, lv, ')');
	AclType aclType;
	if (aclTypeRaw == 'd') {
		aclType = AclType::kDefault;
	} else if (aclTypeRaw == 'a') {
		aclType = AclType::kAccess;
	} else if (aclTypeRaw == 'r') {
		aclType = AclType::kRichACL;
	} else {
		safs_pretty_syslog(LOG_ERR, "%s:%" PRIu64 ": corrupted ACL type", filename, lv);
		return -1;
	}

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	int status =
	    gFSOperations->deleteAcl(FsContext::getForRestore(ts), fsOpContext, inode, aclType);

	if (status == SAUNAFS_STATUS_OK && fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.commitTransaction()) {
			safs::log_err("{}: transaction failed to commit: inode {}, ACL type {}", __func__,
			              inode, aclTypeRaw);
			return SAUNAFS_ERROR_IO;
		}
	}

	return status;
}

int do_setacl(const char *filename, uint64_t lv, uint32_t ts, const char *ptr) {
	inode_t inode;
	char aclType = '\0';
	static uint8_t *aclString = NULL;
	static uint32_t aclSize = 0;

	EAT(ptr, filename, lv, '(');
	GETINODE(inode, ptr);
	EAT(ptr, filename, lv, ',');
	GETCHAR(aclType, ptr);
	EAT(ptr, filename, lv, ',');
	GETPATH(aclString, aclSize, ptr, filename, lv, ')');
	EAT(ptr, filename, lv, ')');

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	int status = gFSOperations->applySetAcl(fsOpContext, ts, inode, aclType,
	                                        reinterpret_cast<const char *>(aclString));

	if (status == SAUNAFS_STATUS_OK && fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.commitTransaction()) {
			safs::log_err("{}: transaction failed to commit: inode {}, ACL type {}", __func__,
			              inode, aclType);
			return SAUNAFS_ERROR_IO;
		}
	}

	return status;
}

int do_setrichacl(const char *filename, uint64_t lv, uint32_t ts, const char *ptr) {
	inode_t inode;
	static uint8_t *acl_string = NULL;
	static uint32_t acl_size = 0;

	EAT(ptr, filename, lv, '(');
	GETINODE(inode, ptr);
	EAT(ptr, filename, lv, ',');
	GETPATH(acl_string, acl_size, ptr, filename, lv, ')');
	EAT(ptr, filename, lv, ')');

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	int status = gFSOperations->applySetRichAcl(fsOpContext, ts, inode,
	                                            reinterpret_cast<const char *>(acl_string));

	if (status == SAUNAFS_STATUS_OK && fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.commitTransaction()) {
			safs::log_err("{}: transaction failed to commit: inode {}", __func__, inode);
			return SAUNAFS_ERROR_IO;
		}
	}

	return status;
}

int do_setquota(const char *filename, uint64_t lv, uint32_t /*ts*/, const char *ptr) {
	char rigor = '\0', resource = '\0', ownerType = '\0';
	inode_t ownerId;
	uint64_t limit;

	EAT(ptr, filename, lv, '(');
	GETCHAR(rigor, ptr);
	EAT(ptr, filename, lv, ',');
	GETCHAR(resource, ptr);
	EAT(ptr, filename, lv, ',');
	GETCHAR(ownerType, ptr);
	EAT(ptr, filename, lv, ',');
	GETINODE(ownerId, ptr);
	EAT(ptr, filename, lv, ',');
	GETU64(limit, ptr);
	EAT(ptr, filename, lv, ')');

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	int status = gFSOperations->applySetQuota(fsOpContext, rigor, resource, ownerType, ownerId,
	                                          limit);

	if (status == SAUNAFS_STATUS_OK && fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.commitTransaction()) {
			safs::log_err("{}: transaction failed to commit: ownerType {}, ownerId {}", __func__,
			              ownerType, ownerId);
			return SAUNAFS_ERROR_IO;
		}
	}

	return status;
}

int do_snapshot(const char* /*filename*/, uint64_t /*lv*/, uint32_t /*ts*/, const char* /*ptr*/) {
	safs_pretty_syslog(LOG_ERR, "Trying to execute deprecated do_snapshot");
	return -1;
}

int do_clone_node(const char* filename, uint64_t lv, uint32_t ts, const char* ptr) {
	inode_t src_inode, dst_parent, dst_inode;
	uint32_t can_overwrite;
	uint8_t name[256];
	EAT(ptr,filename,lv,'(');
	GETINODE(src_inode,ptr);
	EAT(ptr,filename,lv,',');
	GETINODE(dst_parent,ptr);
	EAT(ptr,filename,lv,',');
	GETINODE(dst_inode,ptr);
	EAT(ptr,filename,lv,',');
	GETNAME(name,ptr,filename,lv,',');
	EAT(ptr,filename,lv,',');
	GETU32(can_overwrite,ptr);
	EAT(ptr,filename,lv,')');
	return fs_clone_node(FsContext::getForRestore(ts), src_inode, dst_parent, dst_inode,
				HString((const char*)name), can_overwrite);
}

int do_symlink(const char* filename, uint64_t lv, uint32_t ts, const char* ptr) {
	inode_t parent;
	uint32_t uid,gid;
	inode_t inode;
	uint8_t name[256];
	static uint8_t *path = NULL;
	static uint32_t pathsize = 0;
	EAT(ptr,filename,lv,'(');
	GETINODE(parent,ptr);
	EAT(ptr,filename,lv,',');
	GETNAME(name,ptr,filename,lv,',');
	EAT(ptr,filename,lv,',');
	GETPATH(path,pathsize,ptr,filename,lv,',');
	EAT(ptr,filename,lv,',');
	GETU32(uid,ptr);
	EAT(ptr,filename,lv,',');
	GETU32(gid,ptr);
	EAT(ptr,filename,lv,')');
	EAT(ptr,filename,lv,':');
	GETINODE(inode,ptr);

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	int status = gFSOperations->symlink(FsContext::getForRestoreWithUidGid(ts, uid, gid),
	                                   fsOpContext, parent, HString((char *)name),
	                                   std::string((char *)path), &inode, nullptr);

	if (status == SAUNAFS_STATUS_OK && fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.commitTransaction()) {
			safs::log_err("{}: transaction failed to commit: parent inode {}, name {}, path {}",
			              __func__, parent, reinterpret_cast<const char *>(name),
			              reinterpret_cast<const char *>(path));
			status = SAUNAFS_ERROR_IO;
		}
	}

	return status;
}

int do_undel(const char* filename, uint64_t lv, uint32_t ts, const char* ptr) {
	inode_t inode;
	EAT(ptr,filename,lv,'(');
	GETINODE(inode,ptr);
	EAT(ptr,filename,lv,')');
	return gFSOperations->undel(FsContext::getForRestore(ts), inode);
}

int do_unlink(const char* filename, uint64_t lv, uint32_t ts, const char* ptr) {
	inode_t inode;
	inode_t parent;
	uint8_t name[256];
	EAT(ptr,filename,lv,'(');
	GETINODE(parent,ptr);
	EAT(ptr,filename,lv,',');
	GETNAME(name,ptr,filename,lv,')');
	EAT(ptr,filename,lv,')');
	EAT(ptr,filename,lv,':');
	GETINODE(inode,ptr);
	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	int status = gFSOperations->applyUnlink(fsOpContext, ts, parent, HString((char *)name), inode);

	if (status == SAUNAFS_STATUS_OK && fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.commitTransaction()) {
			safs::log_err("{}: transaction failed to commit: parent {}, inode {}",
			              __func__, parent, inode);
			status = SAUNAFS_ERROR_IO;
		}
	}

	return status;
}

int do_unlock(const char* filename, uint64_t lv, uint32_t ts, const char* ptr) {
	uint64_t chunkid;
	(void)ts;
	EAT(ptr,filename,lv,'(');
	GETU64(chunkid,ptr);
	EAT(ptr,filename,lv,')');
	return gFSOperations->applyUnlock(chunkid);
}

int do_nextchunkid(const char* filename, uint64_t lv, uint32_t ts, const char* ptr) {
	uint64_t nextChunkId;
	EAT(ptr, filename, lv, '(');
	GETU64(nextChunkId, ptr);
	EAT(ptr, filename, lv, ')');
	return gFSOperations->setNextChunkId(FsContext::getForRestore(ts), nextChunkId);
}


int do_trunc(const char* filename, uint64_t lv, uint32_t ts, const char* ptr) {
	inode_t inode;
	uint32_t indx;
	uint32_t lockid;
	uint64_t chunkid;
	EAT(ptr,filename,lv,'(');
	GETINODE(inode,ptr);
	EAT(ptr,filename,lv,',');
	GETU32(indx,ptr);
	if (*ptr==',') {
		EAT(ptr,filename,lv,',');
		GETU32(lockid,ptr);
	} else {
		lockid = 0;
	}
	EAT(ptr,filename,lv,')');
	EAT(ptr,filename,lv,':');
	GETU64(chunkid,ptr);
	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	int status = gFSOperations->applyTrunc(fsOpContext, ts, inode, indx, chunkid, lockid);

	if (status == SAUNAFS_STATUS_OK && fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.commitTransaction()) {
			safs::log_err("{}: transaction failed to commit: inode {}, chunk index {}, chunkid {}",
			              __func__, inode, indx, chunkid);
			status = SAUNAFS_ERROR_IO;
		}
	}

	return status;
}

int do_write(const char* filename, uint64_t lv, uint32_t ts, const char* ptr) {
	inode_t inode;
	uint32_t indx;
	uint64_t chunkid;
	uint32_t lockid;
	uint8_t opflag;
	EAT(ptr,filename,lv,'(');
	GETINODE(inode,ptr);
	EAT(ptr,filename,lv,',');
	GETU32(indx,ptr);
	if (*ptr==',') {
		EAT(ptr,filename,lv,',');
		GETU32(opflag,ptr);
	} else {
		opflag=1;
	}
	if (*ptr==',') {
		EAT(ptr,filename,lv,',');
		GETU32(lockid,ptr);
	} else {
		lockid=1;
	}
	EAT(ptr,filename,lv,')');
	EAT(ptr,filename,lv,':');
	GETU64(chunkid,ptr);

	auto fsOpContext = gFSOperations->createFilesystemOperationContext(
	    FilesystemOperationContext::TransactionType::kReadWrite);

	int status = gFSOperations->writeChunk(FsContext::getForRestore(ts), fsOpContext, inode, indx,
	                                       &lockid, &chunkid, &opflag, nullptr);

	if (status == SAUNAFS_STATUS_OK && fsOpContext.hasReadWriteTransaction()) {
		if (!fsOpContext.commitTransaction()) {
			safs::log_err("{}: transaction failed to commit: inode {}, chunk index {}, chunk id {}",
			              __func__, inode, indx, chunkid);
			status = SAUNAFS_ERROR_IO;
		}
	}

	return status;
}

int restore_line(const char* filename, uint64_t lv, const char* line) {
	uint32_t ts;
	int status;

	status = SAUNAFS_ERROR_MAX;
	const char* ptr = line;

	EAT(ptr,filename,lv,':');
	EAT(ptr,filename,lv,' ');
	GETU32(ts,ptr);
	EAT(ptr,filename,lv,'|');
	switch (*ptr) {
		case 'A':
			if (strncmp(ptr,"ACCESS",6)==0) {
				status = do_access(filename,lv,ts,ptr+6);
			} else if (strncmp(ptr,"ATTR",4)==0) {
				status = do_attr(filename,lv,ts,ptr+4);
			} else if (strncmp(ptr,"APPEND",6)==0) {
				status = do_append(filename,lv,ts,ptr+6);
			} else if (strncmp(ptr,"ACQUIRE",7)==0) {
				status = do_acquire(filename,lv,ts,ptr+7);
			} else if (strncmp(ptr,"AQUIRE",6)==0) { // pragma: codespell-ignore
				status = do_acquire(filename,lv,ts,ptr+6);
			}
			break;
		case 'C':
			if (strncmp(ptr,"CHECKSUM",8)==0) {
				status = do_checksum(filename,lv,ts,ptr+8);
			} else if (strncmp(ptr,"CLONE",5)==0) {
				status = do_clone_node(filename,lv,ts,ptr+5);
			} else if (strncmp(ptr,"CREATE",6)==0) {
				status = do_create(filename,lv,ts,ptr+6);
			} else if (strncmp(ptr,"CUSTOMER",8)==0) {      // deprecated
				status = do_session(filename,lv,ts,ptr+8);
			} else if (strncmp(ptr,"CLRLCK",6)==0) {
				status = do_lock_clear_session(filename,lv,ts,ptr+6);
			}
			break;
		case 'D':
			if (strncmp(ptr,"DELETEACL",9)==0) {
				status = do_deleteacl(filename,lv,ts,ptr+9);
			}
			break;
		case 'F':
			if (strncmp(ptr,"FLCKINODE",9)==0) {
				status = do_lock_unlock_inode(filename,lv,ts,ptr+9);
			} else if (strncmp(ptr, "FLCK", 4) == 0) {
				status = do_lock_op(filename,lv,ts,ptr+4);
			}
			break;
		case 'I':
			if (strncmp(ptr,"INCVERSION",10)==0) {
				status = do_incversion(filename,lv,ts,ptr+10);
			}
			break;
		case 'L':
			if (strncmp(ptr,"LENGTH",6)==0) {
				status = do_length(filename,lv,ts,ptr+6);
			} else if (strncmp(ptr,"LINK",4)==0) {
				status = do_link(filename,lv,ts,ptr+4);
			}
			break;
		case 'M':
			if (strncmp(ptr,"MOVE",4)==0) {
				status = do_move(filename,lv,ts,ptr+4);
			}
			break;
		case 'N':
			if (strncmp(ptr, "NEXTCHUNKID", 11) == 0) {
				status = do_nextchunkid(filename,lv,ts,ptr + 11);
			}
			break;
		case 'P':
			if (strncmp(ptr,"PURGE",5)==0) {
				status = do_purge(filename,lv,ts,ptr+5);
			}
			break;
		case 'R':
			if (strncmp(ptr,"RELEASE",7)==0) {
				status = do_release(filename,lv,ts,ptr+7);
			} else if (strncmp(ptr,"REPAIR",6)==0) {
				status = do_repair(filename,lv,ts,ptr+6);
			} else if (strncmp(ptr,"RMPLOCK",7)==0) {
				status = do_remove_pending_op(filename,lv,ts,ptr+7);
			}
			break;
		case 'S':
			if (strncmp(ptr,"SESSION",7)==0) {
				status = do_session(filename,lv,ts,ptr+7);
			} else if (strncmp(ptr,"SETACL",6)==0) {
				status = do_setacl(filename,lv,ts,ptr+6);
			} else if (strncmp(ptr,"SETEATTR",8)==0) {
				status = do_seteattr(filename,lv,ts,ptr+8);
			} else if (strncmp(ptr,"SETGOAL",7)==0) {
				status = do_setgoal(filename,lv,ts,ptr+7);
			} else if (strncmp(ptr,"SETPATH",7)==0) {
				status = do_setpath(filename,lv,ts,ptr+7);
			} else if (strncmp(ptr,"SETQUOTA",8)==0) {
				status = do_setquota(filename,lv,ts,ptr+8);
			} else if (strncmp(ptr,"SETTRASHTIME",12)==0) {
				status = do_settrashtime(filename,lv,ts,ptr+12);
			} else if (strncmp(ptr,"SETXATTR",8)==0) {
				status = do_setxattr(filename,lv,ts,ptr+8);
			} else if (strncmp(ptr,"SNAPSHOT",8)==0) {    // deprecated
				status = do_snapshot(filename,lv,ts,ptr+8);
			} else if (strncmp(ptr,"SYMLINK",7)==0) {
				status = do_symlink(filename,lv,ts,ptr+7);
			} else if (strncmp(ptr,"SETRICHACL",10)==0) {
				status = do_setrichacl(filename,lv,ts,ptr+10);
			}
			break;
		case 'T':
			if (strncmp(ptr,"TRUNC",5)==0) {
				status = do_trunc(filename,lv,ts,ptr+5);
			}
			break;
		case 'U':
			if (strncmp(ptr,"UNLINK",6)==0) {
				status = do_unlink(filename,lv,ts,ptr+6);
			} else if (strncmp(ptr,"UNDEL",5)==0) {
				status = do_undel(filename,lv,ts,ptr+5);
			} else if (strncmp(ptr,"UNLOCK",6)==0) {
				status = do_unlock(filename,lv,ts,ptr+6);
			}
			break;
		case 'W':
			if (strncmp(ptr,"WRITE",5)==0) {
				status = do_write(filename,lv,ts,ptr+5);
			}
			break;
		default:
			break;
	}

	if (status == SAUNAFS_ERROR_MAX) {
#ifndef METARESTORE
		safs_silent_syslog(LOG_DEBUG, "master.mismatch File %s, %" PRIu64 ", %s -- unknown entry",
			   filename, lv, line);
#endif
		safs_pretty_syslog(LOG_ERR, "%s:%" PRIu64 ": unknown entry '%s'", filename, lv, ptr);
	} else if (status != SAUNAFS_STATUS_OK) {
		uint8_t errorStatus = 0;
		if (status == -1) {
			errorStatus = SAUNAFS_ERROR_PARSE;
		} else if (status > SAUNAFS_ERROR_MAX || status < 0) {
			safs::log_err("restore_line returned error code other than -1 or SAUNAFS_ERROR_CODE, in file {}:{}, status number {}", filename, lv, status);
			assert(false);
			errorStatus = SAUNAFS_ERROR_UNKNOWN;
		} else {
			errorStatus = static_cast<uint8_t>(status);
		}
#ifndef METARESTORE
		safs_silent_syslog(LOG_DEBUG, "master.mismatch File %s, %" PRIu64 ", %s -- %s",
			   filename, lv, line, saunafs_error_string(errorStatus));
#endif
		safs_pretty_syslog(LOG_ERR, "%s:%" PRIu64 ": error: %d (%s)", filename, lv, status,
			saunafs_error_string(errorStatus));
	}
	return status;
}

namespace {

uint64_t nextFsVersion = 0;
uint64_t currentFsVersion = 0; /* Metadata version from before restore_line() call. */
/*
 * By definition:
 *   nextFsVersion == currentFsVersion + 1
 * or:
 *   currentFsVersion == nextFsVersion - 1
 */
const char *lastfn = NULL;
uint8_t verbosity = 0;

}  // namespace

void restore_reset() {
	nextFsVersion = 0;
	currentFsVersion = 0;
	lastfn = NULL;
}

uint8_t restore(const char* filename, uint64_t newLogVersion, const char *ptr, RestoreRigor rigor) {
	if (currentFsVersion == 0 || nextFsVersion == 0) {
		/*
		 * This is first call to restore().
		 */
		nextFsVersion = gFSOperations->getMetadataVersion();
		currentFsVersion = nextFsVersion - 1;
		lastfn = "(no file)";
	}
	if (verbosity > 1) {
		safs_pretty_syslog(LOG_NOTICE, "filename: %s ; current meta version: %" PRIu64 " ; previous changeid: %"
				PRIu64 " ; current changeid: %" PRIu64 " ; change data%s",
				filename, nextFsVersion, currentFsVersion, newLogVersion, ptr);
	}
	if (newLogVersion < currentFsVersion) {
		safs_pretty_syslog(LOG_ERR,
				"merge error - possibly corrupted input file - ignore entry"
				" (filename: %s, versions: %" PRIu64 ", %" PRIu64 ")",
				filename, newLogVersion, currentFsVersion);
		return SAUNAFS_STATUS_OK;
	} else if (newLogVersion >= nextFsVersion) {
		if (newLogVersion == currentFsVersion) {
			if (verbosity > 1) {
				safs_pretty_syslog(LOG_WARNING, "duplicated entry: %" PRIu64 " (previous file: %s, current file: %s)",
						newLogVersion, lastfn, filename);
			}
		} else if (newLogVersion > currentFsVersion + 1) {
			safs_pretty_syslog(LOG_ERR, "hole in change files (entries from %s:%" PRIu64 " to %s:%" PRIu64
					" are missing) - add more files", lastfn, currentFsVersion + 1, filename, newLogVersion - 1);
			return SAUNAFS_ERROR_CHANGELOGINCONSISTENT;
		} else {
			if (verbosity > 0) {
				safs_pretty_syslog(LOG_NOTICE, "%s: change %s", filename, ptr);
			}
			int status = restore_line(filename,newLogVersion,ptr);
			if (status<0) { // parse error - stop processing if requested
				return (rigor == RestoreRigor::kIgnoreParseErrors ? 0 : SAUNAFS_ERROR_PARSE);
			}
			if (status != SAUNAFS_STATUS_OK) { // other errors - stop processing data
				return status;
			}
			nextFsVersion = gFSOperations->getMetadataVersion();
			if ((newLogVersion + 1) != nextFsVersion) {
				/*
				 * restore_line() should bump nextFsVersion by exactly 1, but it didn't.
				 */
				safs_pretty_syslog(LOG_ERR, "%s:%" PRIu64 ":%" PRIu64 " version mismatch", filename, newLogVersion, nextFsVersion);
				return SAUNAFS_ERROR_METADATAVERSIONMISMATCH;
			}
		}
	}
	currentFsVersion = newLogVersion;
	lastfn = filename;
	return SAUNAFS_STATUS_OK;
}

void restore_setverblevel(uint8_t _vlevel) {
	verbosity = _vlevel;
}

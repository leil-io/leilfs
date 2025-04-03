/*

   Copyright 2017 Skytechnology sp. z o.o.
   Copyright 2023 Leil Storage OÜ

   This file is part of SaunaFS.

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

#include "client/sauna_client_c_linkage.h"
#include "common/richacl.h"

#include <boost/intrusive/list.hpp>
#include <mutex>

namespace saunafs {

/*!
 * \brief An object-based wrapper for SaunaClient namespace.
 *
 * Dynamic library hacks are required, because SaunaClient namespace is designed to be a singleton.
 */

class Client {
public:
	typedef SaunaClient::FsInitParams FsInitParams;
	typedef SaunaClient::JobId JobId;
	typedef SaunaClient::NamedInodeOffset NamedInodeOffset;
	typedef SaunaClient::AttrReply AttrReply;
	typedef std::vector<uint8_t> XattrBuffer;
	typedef SaunaClient::DirEntry DirEntry;
	typedef SaunaClient::EntryParam EntryParam;
	typedef SaunaClient::Context Context;
	typedef std::vector<DirEntry> ReadDirReply;
	typedef ReadCache::Result ReadResult;
	typedef std::vector<NamedInodeEntry> ReadReservedReply;
	typedef std::vector<NamedInodeEntry> ReadTrashReply;
	typedef safs_locks::FlockWrapper FlockWrapper;

	struct Stats {
		uint64_t total_space;
		uint64_t avail_space;
		uint64_t trash_space;
		uint64_t reserved_space;
		inode_t inodes;
	};

	struct FileInfo : public SaunaClient::FileInfo, public boost::intrusive::list_base_hook<> {
		FileInfo() {}
		FileInfo(inode_t inode, uint64_t opendirSessionID = 0)
			: inode(inode)
			, opendirSessionID(opendirSessionID) {
		}
		inode_t inode;
		uint64_t opendirSessionID;
	};
	typedef boost::intrusive::list<FileInfo> FileInfoList;

	Client(const std::string &host, const std::string &port, const std::string &mountpoint);
	Client(FsInitParams &params);

	~Client();

	/*! \brief Update groups information */
	void updateGroups(Context &ctx);
	void updateGroups(Context &ctx, std::error_code &ec);

	/*! \brief Find inode in parent directory by name */
	void lookup(Context &ctx, inode_t parent, const std::string &path, EntryParam &param);
	void lookup(Context &ctx, inode_t parent, const std::string &path, EntryParam &param,
	            std::error_code &ec);

	/*! \brief Create a file with given parent and name */
	void mknod(Context &ctx, inode_t parent, const std::string &path, mode_t mode,
	           dev_t rdev, EntryParam &param);
	void mknod(Context &ctx, inode_t parent, const std::string &path, mode_t mode,
	           dev_t rdev, EntryParam &param, std::error_code &ec);

	/*! \brief Create a link with a given parent and name */
	void link(Context &ctx, inode_t inode, inode_t parent,
	             const std::string &name, EntryParam &param);
	void link(Context &ctx, inode_t inode, inode_t parent,
	             const std::string &name, EntryParam &param, std::error_code &ec);

	/*! \brief Create a symbolic link with a given parent and name */
	void symlink(Context &ctx, const std::string &link, inode_t parent,
	             const std::string &name, EntryParam &param);
	void symlink(Context &ctx, const std::string &link, inode_t parent,
	             const std::string &name, EntryParam &param, std::error_code &ec);

	/*! \brief Open a file by inode */
	FileInfo *open(Context &ctx, inode_t inode, int flags);
	FileInfo *open(Context &ctx, inode_t inode, int flags, std::error_code &ec);

	/*! \brief Open a directory by inode */
	FileInfo *opendir(Context &ctx, inode_t ino);
	FileInfo *opendir(Context &ctx, inode_t ino, std::error_code &ec);

	/*! \brief Release a previously open directory */
	void releasedir(FileInfo* fileinfo);
	void releasedir(FileInfo* fileinfo, std::error_code &ec);

	/*! \brief Remove a directory */
	void rmdir(Context &ctx, inode_t parent, const std::string &path);
	void rmdir(Context &ctx, inode_t parent, const std::string &path, std::error_code &ec);

	/*! \brief Read directory contents */
	ReadDirReply readdir(Context &ctx, FileInfo* fileinfo, off_t offset,
	                     size_t max_entries);
	ReadDirReply readdir(Context &ctx, FileInfo* fileinfo, off_t offset,
	                     size_t max_entries, std::error_code &ec);

	/*! \brief Read link contents */
	std::string readlink(Context &ctx, inode_t inode);
	std::string readlink(Context &ctx, inode_t inode, std::error_code &ec);

	/*! \brief Read reserved contents */
	ReadReservedReply readreserved(Context &ctx, NamedInodeOffset offset, NamedInodeOffset max_entries);
	ReadReservedReply readreserved(Context &ctx, NamedInodeOffset offset, NamedInodeOffset max_entries, std::error_code &ec);

	/*! \brief Read trash contents */
	ReadTrashReply readtrash(Context &ctx, NamedInodeOffset offset, NamedInodeOffset max_entries);
	ReadTrashReply readtrash(Context &ctx, NamedInodeOffset offset, NamedInodeOffset max_entries, std::error_code &ec);

	/*! \brief Create a directory */
	void mkdir(Context &ctx, inode_t parent, const std::string &path, mode_t mode,
	           EntryParam &entry_param);
	void mkdir(Context &ctx, inode_t parent, const std::string &path, mode_t mode,
	           EntryParam &entry_param, std::error_code &ec);

	/*! \brief Unlink a file by parent and name entry */
	void unlink(Context &ctx, inode_t parent, const std::string &path);
	void unlink(Context &ctx, inode_t parent, const std::string &path, std::error_code &ec);

	/*! \brief Undelete file from trash */
	void undel(Context &ctx, inode_t ino);
	void undel(Context &ctx, inode_t ino, std::error_code &ec);

	/*! \brief Rename a file */
	void rename(Context &ctx, inode_t parent, const std::string &path, inode_t new_parent,
	            const std::string &new_path);
	void rename(Context &ctx, inode_t parent, const std::string &path, inode_t new_parent,
	            const std::string &new_path, std::error_code &ec);

	/*! \brief Set inode attributes */
	void setattr(Context &ctx, inode_t ino, struct stat *stbuf, int to_set,
	             AttrReply &attr_reply);
	void setattr(Context &ctx, inode_t ino, struct stat *stbuf, int to_set,
	             AttrReply &attr_reply, std::error_code &ec);

	/*! \brief Read bytes from open file, returns read cache result that holds cache lock */
	ReadResult read(Context &ctx, FileInfo *fileinfo, off_t offset, std::size_t size);
	ReadResult read(Context &ctx, FileInfo *fileinfo, off_t offset, std::size_t size,
	                std::error_code &ec);

	/*! \brief Write bytes to open file */
	std::size_t write(Context &ctx, FileInfo *fileinfo, off_t offset, std::size_t size,
	                  const char *buffer);
	std::size_t write(Context &ctx, FileInfo *fileinfo, off_t offset, std::size_t size,
	                  const char *buffer, std::error_code &ec);

	/*! \brief Release a previously open file */
	void release(FileInfo *fileinfo);
	void release(FileInfo *fileinfo, std::error_code &ec);

	/*! \brief Flush data written to an open file */
	void flush(Context &ctx, FileInfo *fileinfo);
	void flush(Context &ctx, FileInfo *fileinfo, std::error_code &ec);

	/*! \brief Get attributes by inode */
	void getattr(Context &ctx, inode_t ino, AttrReply &attr_reply);
	void getattr(Context &ctx, inode_t ino, AttrReply &attr_reply, std::error_code &ec);

	/*! \brief Create a snapshot of a file */
	JobId makesnapshot(Context &ctx, inode_t src_inode, inode_t dst_inode,
	                  const std::string &dst_name, bool can_overwrite);
	JobId makesnapshot(Context &ctx, inode_t src_inode, inode_t dst_inode,
	                  const std::string &dst_name, bool can_overwrite, std::error_code &ec);

	/*! \brief Get replication goal for a file */
	std::string getgoal(Context &ctx, inode_t ino);
	std::string getgoal(Context &ctx, inode_t ino, std::error_code &ec);

	/*! \brief Set replication goal for a file */
	void setgoal(Context &ctx, inode_t inode, const std::string &goal_name, uint8_t smode);
	void setgoal(Context &ctx, inode_t inode, const std::string &goal_name, uint8_t smode,
	             std::error_code &ec);

	void fsync(Context &ctx, FileInfo *fileinfo);
	void fsync(Context &ctx, FileInfo *fileinfo, std::error_code &ec);

	void statfs(Stats &stats);
	void statfs(Stats &stats, std::error_code &ec);

	void setxattr(Context &ctx, inode_t ino, const std::string &name,
	              const XattrBuffer &value, int flags);
	void setxattr(Context &ctx, inode_t ino, const std::string &name,
	              const XattrBuffer &value, int flags, std::error_code &ec);

	XattrBuffer getxattr(Context &ctx, inode_t ino, const std::string &name);
	XattrBuffer getxattr(Context &ctx, inode_t ino, const std::string &name,
	                     std::error_code &ec);

	XattrBuffer listxattr(Context &ctx, inode_t ino);
	XattrBuffer listxattr(Context &ctx, inode_t ino, std::error_code &ec);

	void removexattr(Context &ctx, inode_t ino, const std::string &name);
	void removexattr(Context &ctx, inode_t ino, const std::string &name, std::error_code &ec);

	void setacl(Context &ctx, inode_t ino, const RichACL &acl);
	void setacl(Context &ctx, inode_t ino, const RichACL &acl, std::error_code &ec);

	RichACL getacl(Context &ctx, inode_t ino);
	RichACL getacl(Context &ctx, inode_t ino, std::error_code &ec);

	static std::vector<std::string> toXattrList(const XattrBuffer &buffer);

	std::vector<ChunkWithAddressAndLabel> getchunksinfo(Context &ctx, inode_t ino,
	                                      uint32_t chunk_index, uint32_t chunk_count);
	std::vector<ChunkWithAddressAndLabel> getchunksinfo(Context &ctx, inode_t ino,
	                                      uint32_t chunk_index, uint32_t chunk_count,
	                                      std::error_code &ec);

	std::vector<ChunkserverListEntry> getchunkservers();
	std::vector<ChunkserverListEntry> getchunkservers(std::error_code &ec);

	void getlk(Context &ctx, inode_t ino, FileInfo *fileinfo, FlockWrapper &lock);
	void getlk(Context &ctx, inode_t ino, FileInfo *fileinfo, FlockWrapper &lock,
	           std::error_code &ec);
	void setlk(Context &ctx, inode_t ino, FileInfo *fileinfo, FlockWrapper &lock,
	               std::function<int(const safs_locks::InterruptData &)> handler);
	void setlk(Context &ctx, inode_t ino, FileInfo *fileinfo, FlockWrapper &lock,
	               std::function<int(const safs_locks::InterruptData &)> handler,
	               std::error_code &ec);
	void setlk_interrupt(const safs_locks::InterruptData &data);
	void setlk_interrupt(const safs_locks::InterruptData &data, std::error_code &ec);

protected:
	/*! \brief Initialize client with parameters */
	void init(FsInitParams &params);

	void *linkLibrary();

	typedef decltype(&saunafs_fs_init) FsInitFunction;
	typedef decltype(&saunafs_fs_term) FsTermFunction;
	typedef decltype(&saunafs_lookup) LookupFunction;
	typedef decltype(&saunafs_mknod) MknodFunction;
	typedef decltype(&saunafs_link) LinkFunction;
	typedef decltype(&saunafs_symlink) SymlinkFunction;
	typedef decltype(&saunafs_mkdir) MkDirFunction;
	typedef decltype(&saunafs_rmdir) RmDirFunction;
	typedef decltype(&saunafs_readdir) ReadDirFunction;
	typedef decltype(&saunafs_readlink) ReadLinkFunction;
	typedef decltype(&saunafs_readreserved) ReadReservedFunction;
	typedef decltype(&saunafs_readtrash) ReadTrashFunction;
	typedef decltype(&saunafs_opendir) OpenDirFunction;
	typedef decltype(&saunafs_releasedir) ReleaseDirFunction;
	typedef decltype(&saunafs_unlink) UnlinkFunction;
	typedef decltype(&saunafs_undel) UndelFunction;
	typedef decltype(&saunafs_open) OpenFunction;
	typedef decltype(&saunafs_setattr) SetAttrFunction;
	typedef decltype(&saunafs_getattr) GetAttrFunction;
	typedef decltype(&saunafs_read) ReadFunction;
	typedef decltype(&saunafs_read_special_inode) ReadSpecialInodeFunction;
	typedef decltype(&saunafs_write) WriteFunction;
	typedef decltype(&saunafs_release) ReleaseFunction;
	typedef decltype(&saunafs_flush) FlushFunction;
	typedef decltype(&saunafs_isSpecialInode) IsSpecialInodeFunction;
	typedef decltype(&saunafs_update_groups) UpdateGroupsFunction;
	typedef decltype(&saunafs_makesnapshot) MakesnapshotFunction;
	typedef decltype(&saunafs_getgoal) GetGoalFunction;
	typedef decltype(&saunafs_setgoal) SetGoalFunction;
	typedef decltype(&saunafs_fsync) FsyncFunction;
	typedef decltype(&saunafs_rename) RenameFunction;
	typedef decltype(&saunafs_statfs) StatfsFunction;
	typedef decltype(&saunafs_setxattr) SetXattrFunction;
	typedef decltype(&saunafs_getxattr) GetXattrFunction;
	typedef decltype(&saunafs_listxattr) ListXattrFunction;
	typedef decltype(&saunafs_removexattr) RemoveXattrFunction;
	typedef decltype(&saunafs_getchunksinfo) GetChunksInfoFunction;
	typedef decltype(&saunafs_getchunkservers) GetChunkserversFunction;
	typedef decltype(&saunafs_getlk) GetlkFunction;
	typedef decltype(&saunafs_setlk_send) SetlkSendFunction;
	typedef decltype(&saunafs_setlk_recv) SetlkRecvFunction;
	typedef decltype(&saunafs_setlk_interrupt) SetlkInterruptFunction;

	FsInitFunction saunafs_fs_init_;
	FsTermFunction saunafs_fs_term_;
	LookupFunction saunafs_lookup_;
	MknodFunction saunafs_mknod_;
	MkDirFunction saunafs_mkdir_;
	LinkFunction saunafs_link_;
	SymlinkFunction saunafs_symlink_;
	RmDirFunction saunafs_rmdir_;
	ReadDirFunction saunafs_readdir_;
	ReadLinkFunction saunafs_readlink_;
	ReadReservedFunction saunafs_readreserved_;
	ReadTrashFunction saunafs_readtrash_;
	OpenDirFunction saunafs_opendir_;
	ReleaseDirFunction saunafs_releasedir_;
	UnlinkFunction saunafs_unlink_;
	UndelFunction saunafs_undel_;
	OpenFunction saunafs_open_;
	SetAttrFunction saunafs_setattr_;
	GetAttrFunction saunafs_getattr_;
	ReadFunction saunafs_read_;
	ReadSpecialInodeFunction saunafs_read_special_inode_;
	WriteFunction saunafs_write_;
	ReleaseFunction saunafs_release_;
	FlushFunction saunafs_flush_;
	IsSpecialInodeFunction saunafs_isSpecialInode_;
	UpdateGroupsFunction saunafs_update_groups_;
	MakesnapshotFunction saunafs_makesnapshot_;
	GetGoalFunction saunafs_getgoal_;
	SetGoalFunction saunafs_setgoal_;
	FsyncFunction saunafs_fsync_;
	RenameFunction saunafs_rename_;
	StatfsFunction saunafs_statfs_;
	SetXattrFunction saunafs_setxattr_;
	GetXattrFunction saunafs_getxattr_;
	ListXattrFunction saunafs_listxattr_;
	RemoveXattrFunction saunafs_removexattr_;
	GetChunksInfoFunction saunafs_getchunksinfo_;
	GetChunkserversFunction saunafs_getchunkservers_;
	GetlkFunction saunafs_getlk_;
	SetlkSendFunction saunafs_setlk_send_;
	SetlkRecvFunction saunafs_setlk_recv_;
	SetlkInterruptFunction saunafs_setlk_interrupt_;

	void *dl_handle_;
	FileInfoList fileinfos_;
	std::mutex mutex_;
	std::atomic<uint64_t> nextOpendirSessionID_;

	static std::atomic<int> instance_count_;

	static constexpr int kMaxXattrRequestSize = 65536;
	static constexpr const char *kLibraryPath = LIB_PATH "/libsaunafsmount_shared.so";
};

} // namespace saunafs

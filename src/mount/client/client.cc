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

#include "common/platform.h"

#include "client.h"

#include <dlfcn.h>
#include <fstream>

#include "client_error_code.h"
#include "common/richacl_converter.h"

#define SAUNAFS_LINK_FUNCTION(function_name) do { \
	void *function_name##_ptr = dlsym(dl_handle_, #function_name); \
	function_name##_ = *(decltype(function_name##_)*)&function_name##_ptr; \
	if (function_name##_ == nullptr) { \
		throw std::runtime_error(std::string("dl lookup failed for ") + #function_name); \
	} \
} while (0)

using namespace saunafs;

static const char *kRichAclXattrName = "system.richacl";

std::atomic<int> Client::instance_count_(0);

void *Client::linkLibrary() {
	void *ret;

	// Special case for the first instance - no copying needed
	if (instance_count_++ == 0) {
		ret = dlopen(kLibraryPath, RTLD_NOW);
		if (ret == nullptr) {
			instance_count_--;
			throw std::runtime_error(std::string("Cannot link: ") + dlerror());
		}
		return ret;
	}

	char pattern[] = "/tmp/libsaunafsmount_shared.so.XXXXXX";
	int out_fd = ::mkstemp(pattern);
	if (out_fd < 0) {
		instance_count_--;
		throw std::runtime_error("Cannot create temporary file");
	}

	std::ifstream source(kLibraryPath);
	std::ofstream dest(pattern);

	dest << source.rdbuf();

	source.close();
	dest.close();
	ret = dlopen(pattern, RTLD_NOW);
	::close(out_fd);
	::unlink(pattern);
	if (ret == nullptr) {
		instance_count_--;
		throw std::runtime_error(std::string("Cannot link: ") + dlerror());
	}
	return ret;
}

Client::Client(const std::string &host, const std::string &port, const std::string &mountpoint)
	: nextOpendirSessionID_(1) {
	FsInitParams params("", host, port, mountpoint);
	init(params);
}

Client::Client(FsInitParams &params)
	: nextOpendirSessionID_(1) {
	init(params);
}

Client::~Client() {
	assert(instance_count_ >= 1);
	assert(dl_handle_);

	while (!fileinfos_.empty()) {
		release(std::addressof(fileinfos_.front()));
	}

	saunafs_fs_term_();
	dlclose(dl_handle_);
	instance_count_--;
}

void Client::init(FsInitParams &params) {
	dl_handle_ = linkLibrary();
	try {
		SAUNAFS_LINK_FUNCTION(saunafs_fs_init);
		SAUNAFS_LINK_FUNCTION(saunafs_fs_term);
		SAUNAFS_LINK_FUNCTION(saunafs_lookup);
		SAUNAFS_LINK_FUNCTION(saunafs_mknod);
		SAUNAFS_LINK_FUNCTION(saunafs_link);
		SAUNAFS_LINK_FUNCTION(saunafs_symlink);
		SAUNAFS_LINK_FUNCTION(saunafs_mkdir);
		SAUNAFS_LINK_FUNCTION(saunafs_rmdir);
		SAUNAFS_LINK_FUNCTION(saunafs_readdir);
		SAUNAFS_LINK_FUNCTION(saunafs_readlink);
		SAUNAFS_LINK_FUNCTION(saunafs_readreserved);
		SAUNAFS_LINK_FUNCTION(saunafs_readtrash);
		SAUNAFS_LINK_FUNCTION(saunafs_opendir);
		SAUNAFS_LINK_FUNCTION(saunafs_releasedir);
		SAUNAFS_LINK_FUNCTION(saunafs_unlink);
		SAUNAFS_LINK_FUNCTION(saunafs_undel);
		SAUNAFS_LINK_FUNCTION(saunafs_open);
		SAUNAFS_LINK_FUNCTION(saunafs_setattr);
		SAUNAFS_LINK_FUNCTION(saunafs_getattr);
		SAUNAFS_LINK_FUNCTION(saunafs_read);
		SAUNAFS_LINK_FUNCTION(saunafs_read_special_inode);
		SAUNAFS_LINK_FUNCTION(saunafs_write);
		SAUNAFS_LINK_FUNCTION(saunafs_release);
		SAUNAFS_LINK_FUNCTION(saunafs_flush);
		SAUNAFS_LINK_FUNCTION(saunafs_isSpecialInode);
		SAUNAFS_LINK_FUNCTION(saunafs_update_groups);
		SAUNAFS_LINK_FUNCTION(saunafs_makesnapshot);
		SAUNAFS_LINK_FUNCTION(saunafs_getgoal);
		SAUNAFS_LINK_FUNCTION(saunafs_setgoal);
		SAUNAFS_LINK_FUNCTION(saunafs_fsync);
		SAUNAFS_LINK_FUNCTION(saunafs_rename);
		SAUNAFS_LINK_FUNCTION(saunafs_statfs);
		SAUNAFS_LINK_FUNCTION(saunafs_setxattr);
		SAUNAFS_LINK_FUNCTION(saunafs_getxattr);
		SAUNAFS_LINK_FUNCTION(saunafs_listxattr);
		SAUNAFS_LINK_FUNCTION(saunafs_removexattr);
		SAUNAFS_LINK_FUNCTION(saunafs_getchunksinfo);
		SAUNAFS_LINK_FUNCTION(saunafs_getchunkservers);
		SAUNAFS_LINK_FUNCTION(saunafs_getlk);
		SAUNAFS_LINK_FUNCTION(saunafs_setlk_send);
		SAUNAFS_LINK_FUNCTION(saunafs_setlk_recv);
		SAUNAFS_LINK_FUNCTION(saunafs_setlk_interrupt);
	} catch (const std::runtime_error &e) {
		dlclose(dl_handle_);
		instance_count_--;
		throw e;
	}

	if (saunafs_fs_init_(params) != 0) {
		assert(dl_handle_);
		dlclose(dl_handle_);
		instance_count_--;
		throw std::runtime_error("Can't connect to master server");
	}
}

void Client::updateGroups(Context &ctx) {
	std::error_code ec;
	updateGroups(ctx, ec);
	if (ec) {
		throw std::system_error(ec);
	}
}

void Client::updateGroups(Context &ctx, std::error_code &ec) {
	auto ret = saunafs_update_groups_(ctx);
	ec = make_error_code(ret);
}


void Client::lookup(Context &ctx, inode_t parent, const std::string &path, EntryParam &param) {
	std::error_code ec;
	lookup(ctx, parent, path, param, ec);
	if (ec) {
		throw std::system_error(ec);
	}
}

void Client::lookup(Context &ctx, inode_t parent, const std::string &path, EntryParam &param,
		std::error_code &ec) {
	int ret = saunafs_lookup_(ctx, parent, path.c_str(), param);
	ec = make_error_code(ret);
}

void Client::mknod(Context &ctx, inode_t parent, const std::string &path, mode_t mode,
		dev_t rdev, EntryParam &param) {
	std::error_code ec;
	mknod(ctx, parent, path, mode, rdev, param, ec);
	if (ec) {
		throw std::system_error(ec);
	}
}

void Client::mknod(Context &ctx, inode_t parent, const std::string &path, mode_t mode,
		dev_t rdev, EntryParam &param, std::error_code &ec) {
	int ret = saunafs_mknod_(ctx, parent, path.c_str(), mode, rdev, param);
	ec = make_error_code(ret);
}

void Client::link(Context &ctx, inode_t inode, inode_t parent,
		const std::string &name, EntryParam &param) {
	std::error_code ec;
	link(ctx, inode, parent, name, param, ec);
	if (ec) {
		throw std::system_error(ec);
	}
}

void Client::link(Context &ctx, inode_t inode, inode_t parent,
		const std::string &name, EntryParam &param, std::error_code &ec) {
	int ret = saunafs_link_(ctx, inode, parent, name.c_str(), param);
	ec = make_error_code(ret);
}

void Client::symlink(Context &ctx, const std::string &link, inode_t parent,
		const std::string &name, EntryParam &param) {
	std::error_code ec;
	symlink(ctx, link, parent, name, param, ec);
	if (ec) {
		throw std::system_error(ec);
	}
}

void Client::symlink(Context &ctx, const std::string &link, inode_t parent,
		const std::string &name, EntryParam &param, std::error_code &ec) {
	int ret = saunafs_symlink_(ctx, link.c_str(), parent, name.c_str(), param);
	ec = make_error_code(ret);
}

Client::ReadDirReply Client::readdir(Context &ctx, FileInfo* fileinfo, off_t offset,
		size_t max_entries) {
	std::error_code ec;
	auto dir_entries = readdir(ctx, fileinfo, offset, max_entries, ec);
	if (ec) {
		throw std::system_error(ec);
	}
	return dir_entries;
}

Client::ReadDirReply Client::readdir(Context &ctx, FileInfo* fileinfo, off_t offset,
		size_t max_entries, std::error_code &ec) {
	std::vector<SaunaClient::DirEntry> entries;
	auto ret = saunafs_readdir_(ctx, fileinfo->opendirSessionID,
	                            fileinfo->inode, offset, max_entries, entries);
	ec = make_error_code(ret);
	return entries;
}

std::string Client::readlink(Context &ctx, inode_t inode) {
	std::error_code ec;
	std::string link = readlink(ctx, inode, ec);
	if (ec) {
		throw std::system_error(ec);
	}
	return link;
}

std::string Client::readlink(Context &ctx, inode_t inode, std::error_code &ec) {
	std::string link;
	int ret = saunafs_readlink_(ctx, inode, link);
	ec = make_error_code(ret);
	return link;
}

Client::ReadReservedReply Client::readreserved(Context &ctx, NamedInodeOffset offset,
	                                       NamedInodeOffset max_entries) {
	std::error_code ec;
	auto reserved_entries = readreserved(ctx, offset, max_entries, ec);
	if (ec) {
		throw std::system_error(ec);
	}
	return reserved_entries;
}

Client::ReadReservedReply Client::readreserved(Context &ctx, NamedInodeOffset offset,
	                                       NamedInodeOffset max_entries, std::error_code &ec) {
	std::vector<NamedInodeEntry> inode_entries;
	auto ret = saunafs_readreserved_(ctx, offset, max_entries, inode_entries);
	ec = make_error_code(ret);
	return inode_entries;
}

Client::ReadTrashReply Client::readtrash(Context &ctx, NamedInodeOffset offset,
	                                 NamedInodeOffset max_entries) {
	std::error_code ec;
	auto trash_entries = readtrash(ctx, offset, max_entries, ec);
	if (ec) {
		throw std::system_error(ec);
	}
	return trash_entries;
}

Client::ReadTrashReply Client::readtrash(Context &ctx, NamedInodeOffset offset,
	                                 NamedInodeOffset max_entries, std::error_code &ec) {
	std::vector<NamedInodeEntry> trash_entries;
	auto ret = saunafs_readtrash_(ctx, offset, max_entries, trash_entries);
	ec = make_error_code(ret);
	return trash_entries;
}

Client::FileInfo *Client::opendir(Context &ctx, inode_t inode) {
	std::error_code ec;
	auto fileinfo = opendir(ctx, inode, ec);
	if (ec) {
		assert(!fileinfo);
		throw std::system_error(ec);
	}
	return fileinfo;
}

Client::FileInfo *Client::opendir(Context &ctx, inode_t inode, std::error_code &ec) {
	int ret = saunafs_opendir_(ctx, inode);
	ec = make_error_code(ret);
	if (ec) {
		return nullptr;
	}
	FileInfo *fileinfo = new FileInfo(inode, nextOpendirSessionID_++);
	SaunaClient::update_readdir_session(fileinfo->opendirSessionID, 0);
	std::lock_guard<std::mutex> guard(mutex_);
	fileinfos_.push_front(*fileinfo);
	return fileinfo;
}

void Client::releasedir(FileInfo* fileinfo) {
	std::error_code ec;
	releasedir(fileinfo, ec);
	if (ec) {
		throw std::system_error(ec);
	}
}

void Client::releasedir(FileInfo* fileinfo, std::error_code &ec) {
	assert(fileinfo != nullptr);
	int ret = saunafs_releasedir_(fileinfo->inode, fileinfo->opendirSessionID);
	ec = make_error_code(ret);
	{
		std::lock_guard<std::mutex> guard(mutex_);
		fileinfos_.erase(fileinfos_.iterator_to(*fileinfo));
	}
	delete fileinfo;
}

void Client::rmdir(Context &ctx, inode_t parent, const std::string &path) {
	std::error_code ec;
	rmdir(ctx, parent, path, ec);
	if (ec) {
		throw std::system_error(ec);
	}
}

void Client::rmdir(Context &ctx, inode_t parent, const std::string &path, std::error_code &ec) {
	int ret = saunafs_rmdir_(ctx, parent, path.c_str());
	ec = make_error_code(ret);
}

void Client::mkdir(Context &ctx, inode_t parent, const std::string &path, mode_t mode,
	          Client::EntryParam &entry_param) {
	std::error_code ec;
	mkdir(ctx, parent, path, mode, entry_param, ec);
}

void Client::mkdir(Context &ctx, inode_t parent, const std::string &path, mode_t mode,
	          Client::EntryParam &entry_param, std::error_code &ec) {
	int ret = saunafs_mkdir_(ctx, parent, path.c_str(), mode, entry_param);
	ec = make_error_code(ret);
}

void Client::unlink(Context &ctx, inode_t parent, const std::string &path) {
	std::error_code ec;
	unlink(ctx, parent, path, ec);
	if (ec) {
		throw std::system_error(ec);
	}
}

void Client::unlink(Context &ctx, inode_t parent, const std::string &path, std::error_code &ec) {
	int ret = saunafs_unlink_(ctx, parent, path.c_str());
	ec = make_error_code(ret);
}

void Client::undel(Context &ctx, inode_t ino) {
	std::error_code ec;
	undel(ctx, ino, ec);
	if (ec) {
		throw std::system_error(ec);
	}
}

void Client::undel(Context &ctx, inode_t ino, std::error_code &ec) {
	int ret = saunafs_undel_(ctx, ino);
	ec = make_error_code(ret);
}

void Client::rename(Context &ctx, inode_t parent, const std::string &path, inode_t newparent,
	            const std::string &new_path) {
	std::error_code ec;
	rename(ctx, parent, path, newparent, new_path, ec);
	if (ec) {
		throw std::system_error(ec);
	}
}

void Client::rename(Context &ctx, inode_t parent, const std::string &path, inode_t newparent,
	            const std::string &new_path, std::error_code &ec) {
	int ret = saunafs_rename_(ctx, parent, path.c_str(), newparent, new_path.c_str());
	ec = make_error_code(ret);
}

Client::FileInfo *Client::open(Context &ctx, inode_t inode, int flags) {
	std::error_code ec;
	auto fileinfo = open(ctx, inode, flags, ec);
	if (ec) {
		assert(!fileinfo);
		throw std::system_error(ec);
	}
	return fileinfo;
}

Client::FileInfo *Client::open(Context &ctx, inode_t inode, int flags, std::error_code &ec) {
	FileInfo *fileinfo = new FileInfo(inode);
	fileinfo->flags = flags;

	int ret = saunafs_open_(ctx, inode, fileinfo);
	ec = make_error_code(ret);
	if (ec) {
		delete fileinfo;
		return nullptr;
	}
	std::lock_guard<std::mutex> guard(mutex_);
	fileinfos_.push_front(*fileinfo);
	return fileinfo;
}

void Client::getattr(Context &ctx, inode_t inode, AttrReply &attr_reply) {
	std::error_code ec;
	getattr(ctx, inode, attr_reply, ec);
	if (ec) {
		throw std::system_error(ec);
	}
}

void Client::getattr(Context &ctx, inode_t inode, AttrReply &attr_reply,
		std::error_code &ec) {
	int ret = saunafs_getattr_(ctx, inode, attr_reply);
	ec = make_error_code(ret);
}

void Client::setattr(Context &ctx, inode_t ino, struct stat *stbuf, int to_set,
	             AttrReply &attr_reply) {
	std::error_code ec;
	setattr(ctx, ino, stbuf, to_set, attr_reply, ec);
	if (ec) {
		throw std::system_error(ec);
	}
}

void Client::setattr(Context &ctx, inode_t ino, struct stat *stbuf, int to_set,
	             AttrReply &attr_reply, std::error_code &ec) {
	int ret = saunafs_setattr_(ctx, ino, stbuf, to_set, attr_reply);
	ec = make_error_code(ret);
}

Client::ReadResult Client::read(Context &ctx, FileInfo *fileinfo,
	                       off_t offset, std::size_t size) {
	std::error_code ec;
	auto ret = read(ctx, fileinfo, offset, size, ec);
	if (ec) {
		throw std::system_error(ec);
	}
	return ret;
}

Client::ReadResult Client::read(Context &ctx, FileInfo *fileinfo,
	                       off_t offset, std::size_t size, std::error_code &ec) {
	if (saunafs_isSpecialInode_(fileinfo->inode)) {
		std::vector<uint8_t> special_inode;
		auto ret = saunafs_read_special_inode_(ctx, fileinfo->inode, size,
		                                       offset, fileinfo, special_inode);
		ec = make_error_code(ret);
		if (ec) { return {}; }
		return {std::move(special_inode)};
	}

	ReadCache::Result result;
	int ret =
	    saunafs_read_(ctx, fileinfo->inode, size, offset, fileinfo, result);
	ec = make_error_code(ret);
	if (ec) { return {}; }
	return result;
}

std::size_t Client::write(Context &ctx, FileInfo *fileinfo, off_t offset, std::size_t size,
		const char *buffer) {
	std::error_code ec;
	auto write_size = write(ctx, fileinfo, offset, size, buffer, ec);
	if (ec) {
		throw std::system_error(ec);
	}
	return write_size;
}

std::size_t Client::write(Context &ctx, FileInfo *fileinfo, off_t offset, std::size_t size,
		const char *buffer, std::error_code &ec) {
	ssize_t bytes_written = 0;
	int ret = saunafs_write_(ctx, fileinfo->inode, buffer, size, offset,
	                         fileinfo, bytes_written);
	ec = make_error_code(ret);
	return ec ? (std::size_t)0 : (std::size_t)bytes_written;
}

void Client::release(FileInfo *fileinfo) {
	std::error_code ec;
	release(fileinfo, ec);
	if (ec) {
		throw std::system_error(ec);
	}
}

void Client::release(FileInfo *fileinfo, std::error_code &ec) {
	int ret = saunafs_release_(fileinfo->inode, fileinfo);
	std::lock_guard<std::mutex> guard(mutex_);
	fileinfos_.erase(fileinfos_.iterator_to(*fileinfo));
	delete fileinfo;
	ec = make_error_code(ret);
}

void Client::flush(Context &ctx, FileInfo *fileinfo) {
	std::error_code ec;
	flush(ctx, fileinfo, ec);
	if (ec) {
		throw std::system_error(ec);
	}
}

void Client::flush(Context &ctx, FileInfo *fileinfo, std::error_code &ec) {
	int ret = saunafs_flush_(ctx, fileinfo->inode, fileinfo);
	ec = make_error_code(ret);
}

void Client::fsync(Context &ctx, FileInfo *fileinfo) {
	std::error_code ec;
	fsync(ctx, fileinfo, ec);
	if (ec) {
		throw std::system_error(ec);
	}
}

void Client::fsync(Context &ctx, FileInfo *fileinfo, std::error_code &ec) {
	int ret = saunafs_fsync_(ctx, fileinfo->inode, 0, fileinfo);
	ec = make_error_code(ret);
}

SaunaClient::JobId Client::makesnapshot(Context &ctx, inode_t src_inode, inode_t dst_inode,
	                                 const std::string &dst_name, bool can_overwrite) {
	std::error_code ec;
	JobId job_id = makesnapshot(ctx, src_inode, dst_inode, dst_name, can_overwrite, ec);
	if (ec) {
		throw std::system_error(ec);
	}
	return job_id;
}

SaunaClient::JobId Client::makesnapshot(Context &ctx, inode_t src_inode, inode_t dst_inode,
	                                 const std::string &dst_name, bool can_overwrite,
	                                 std::error_code &ec) {
	SaunaClient::JobId job_id = 0;
	auto ret = saunafs_makesnapshot_(ctx, src_inode, dst_inode, dst_name,
	                                 can_overwrite, job_id);
	ec = make_error_code(ret);
	return job_id;
}

std::string Client::getgoal(Context &ctx, inode_t inode) {
	std::error_code ec;
	std::string res = getgoal(ctx, inode, ec);
	if (ec) {
		throw std::system_error(ec);
	}
	return res;
}

std::string Client::getgoal(Context &ctx, inode_t inode, std::error_code &ec) {
	std::string goal;
	int ret = saunafs_getgoal_(ctx, inode, goal);
	ec = make_error_code(ret);
	return goal;
}

void Client::setgoal(Context &ctx, inode_t inode, const std::string &goal_name, uint8_t smode) {
	std::error_code ec;
	setgoal(ctx, inode, goal_name, smode, ec);
	if (ec) {
		throw std::system_error(ec);
	}
}

void Client::setgoal(Context &ctx, inode_t inode, const std::string &goal_name,
	             uint8_t smode, std::error_code &ec) {
	int ret = saunafs_setgoal_(ctx, inode, goal_name, smode);
	ec = make_error_code(ret);
}

void Client::statfs(Stats &stats) {
	std::error_code ec;
	statfs(stats, ec);
	if (ec) {
		throw std::system_error(ec);
	}
}

void Client::statfs(Stats &stats, std::error_code &ec) {
	int ret = saunafs_statfs_(&stats.total_space, &stats.avail_space, &stats.trash_space,
	                 &stats.reserved_space, &stats.inodes);
	ec = make_error_code(ret);
}

void Client::setxattr(Context &ctx, inode_t ino, const std::string &name,
	             const XattrBuffer &value, int flags) {
	std::error_code ec;
	setxattr(ctx, ino, name, value, flags, ec);
	if (ec) {
		throw std::system_error(ec);
	}
}

void Client::setxattr(Context &ctx, inode_t ino, const std::string &name,
	              const XattrBuffer &value, int flags, std::error_code &ec) {
	int ret = saunafs_setxattr_(ctx, ino, name.c_str(),
	                             (const char *)value.data(), value.size(), flags);
	ec = make_error_code(ret);
}

Client::XattrBuffer Client::getxattr(Context &ctx, inode_t ino, const std::string &name) {
	std::error_code ec;
	auto ret = getxattr(ctx, ino, name, ec);
	if (ec) {
		throw std::system_error(ec);
	}
	return ret;
}

Client::XattrBuffer Client::getxattr(Context &ctx, inode_t ino, const std::string &name,
	                                  std::error_code &ec) {
	SaunaClient::XattrReply reply;
	int ret = saunafs_getxattr_(ctx, ino, name.c_str(), kMaxXattrRequestSize, reply);
	ec = make_error_code(ret);
	return reply.valueBuffer;
}

Client::XattrBuffer Client::listxattr(Context &ctx, inode_t ino) {
	std::error_code ec;
	auto ret = listxattr(ctx, ino, ec);
	if (ec) {
		throw std::system_error(ec);
	}
	return ret;
}

Client::XattrBuffer Client::listxattr(Context &ctx, inode_t ino, std::error_code &ec) {
	SaunaClient::XattrReply reply;
	int ret = saunafs_listxattr_(ctx, ino, kMaxXattrRequestSize, reply);
	ec = make_error_code(ret);
	return reply.valueBuffer;
}

void Client::removexattr(Context &ctx, inode_t ino, const std::string &name) {
	std::error_code ec;
	removexattr(ctx, ino, name, ec);
	if (ec) {
		throw std::system_error(ec);
	}
}

void Client::removexattr(Context &ctx, inode_t ino, const std::string &name, std::error_code &ec) {
	int ret = saunafs_removexattr_(ctx, ino, name.c_str());
	ec = make_error_code(ret);
}

void Client::setacl(Context &ctx, inode_t ino, const RichACL &acl) {
	std::error_code ec;
	setacl(ctx, ino, std::move(acl), ec);
	if (ec) {
		throw std::system_error(ec);
	}
}

void Client::setacl(Context &ctx, inode_t ino, const RichACL &acl, std::error_code &ec) {
	try {
		std::vector<uint8_t> xattr = richAclConverter::objectToRichACLXattr(acl);
		setxattr(ctx, ino, kRichAclXattrName, xattr, 0, ec);
	} catch (...) {
		ec = make_error_code(SAUNAFS_ERROR_ENOATTR);
	}
}

RichACL Client::getacl(Context &ctx, inode_t ino) {
	std::error_code ec;
	auto ret = getacl(ctx, ino, ec);
	if (ec) {
		throw std::system_error(ec);
	}
	return ret;
}

RichACL Client::getacl(Context &ctx, inode_t ino, std::error_code &ec) {
	try {
		auto buffer = getxattr(ctx, ino, kRichAclXattrName, ec);
		if (ec) {
			return RichACL();
		}
		return richAclConverter::extractObjectFromRichACL(buffer.data(), buffer.size());
	} catch (...) {
		ec = make_error_code(SAUNAFS_ERROR_ENOATTR);
	}
	return RichACL();
}

std::vector<std::string> Client::toXattrList(const XattrBuffer &buffer) {
	std::vector<std::string> xattr_list;
	size_t base = 0;
	size_t length = 0;
	while (base < buffer.size()) {
		while (base + length < buffer.size() && buffer[base + length] != '\0') {
			length++;
		}
		if (base + length == buffer.size()) {
			break;
		}
		xattr_list.push_back(std::string((const char *)buffer.data() + base, length));
		base += length + 1;
		length = 0;
	}
	return xattr_list;
}

std::vector<ChunkWithAddressAndLabel> Client::getchunksinfo(Context &ctx, inode_t ino,
	                                          uint32_t chunk_index, uint32_t chunk_count) {
	std::error_code ec;
	auto ret = getchunksinfo(ctx, ino, chunk_index, chunk_count, ec);
	if (ec) {
		throw std::system_error(ec);
	}
	return ret;
}

std::vector<ChunkWithAddressAndLabel> Client::getchunksinfo(Context &ctx, inode_t ino,
	                             uint32_t chunk_index, uint32_t chunk_count, std::error_code &ec) {
	std::vector<ChunkWithAddressAndLabel> chunks;
	auto ret =
	    saunafs_getchunksinfo_(ctx, ino, chunk_index, chunk_count, chunks);
	ec = make_error_code(ret);
	return chunks;
}

std::vector<ChunkserverListEntry> Client::getchunkservers() {
	std::error_code ec;
	auto ret = getchunkservers(ec);
	if (ec) {
		throw std::system_error(ec);
	}
	return ret;
}

std::vector<ChunkserverListEntry> Client::getchunkservers(std::error_code &ec) {
	std::vector<ChunkserverListEntry> chunkservers;
	auto ret = saunafs_getchunkservers_(chunkservers);
	ec = make_error_code(ret);
	return chunkservers;
}

void Client::getlk(Context &ctx, inode_t ino, FileInfo *fileinfo, FlockWrapper &lock) {
	std::error_code ec;
	getlk(ctx, ino, fileinfo, lock, ec);
	if (ec) {
		throw std::system_error(ec);
	}
}

void Client::getlk(Context &ctx, inode_t ino, FileInfo *fileinfo, FlockWrapper &lock,
		std::error_code &ec) {
	int ret = saunafs_getlk_(ctx, ino, fileinfo, lock);
	ec = make_error_code(ret);
}

void Client::setlk(Context &ctx, inode_t ino, FileInfo *fileinfo, FlockWrapper &lock,
	           std::function<int(const safs_locks::InterruptData &)> handler) {
	std::error_code ec;
	setlk(ctx, ino, fileinfo, lock, handler, ec);
	if (ec) {
		throw std::system_error(ec);
	}
}

void Client::setlk(Context &ctx, inode_t ino, FileInfo *fileinfo, FlockWrapper &lock,
	                    std::function<int(const safs_locks::InterruptData &)> handler,
	                    std::error_code &ec) {
	uint32_t reqid = 0;
	auto ret = saunafs_setlk_send_(ctx, ino, fileinfo, lock, reqid);
	ec = make_error_code(ret);
	if (ec) {
		return;
	}
	safs_locks::InterruptData interrupt_data(fileinfo->lock_owner, ino, reqid);
	if (handler) {
		int err = handler(interrupt_data);
		if (err != SAUNAFS_STATUS_OK) {
			ec = make_error_code(err);
			return;
		}
	}
	int err = saunafs_setlk_recv_();
	ec = make_error_code(err);
}

void Client::setlk_interrupt(const safs_locks::InterruptData &data) {
	std::error_code ec;
	setlk_interrupt(data, ec);
	if (ec) {
		throw std::system_error(ec);
	}
}

void Client::setlk_interrupt(const safs_locks::InterruptData &data, std::error_code &ec) {
	int ret = saunafs_setlk_interrupt_(data);
	ec = make_error_code(ret);
}

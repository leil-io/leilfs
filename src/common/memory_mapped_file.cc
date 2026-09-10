/*
	Copyright 2023-2024 Leil Storage OÜ

	LeilFS is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, version 3.

	LeilFS is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with LeilFS  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/platform.h"
#include "memory_mapped_file.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdexcept>
#include <vector>

#include "slogger/slogger.h"
#include "cwrap.h"

class MemoryMappedFile::Impl {
public:
	explicit Impl(const std::string &path);
	virtual ~Impl();

	/// We don't want to allow copying or moving
	Impl(const Impl &) = delete;
	Impl &operator=(const Impl &) = delete;
	Impl(Impl &&) noexcept = delete;
	Impl &operator=(Impl &&) noexcept = delete;

	[[nodiscard]] uint8_t *seek(size_t offset) const;

	size_t read(size_t &offset, uint8_t *buf, size_t size,
	            bool moveOffset = false) const;

	[[nodiscard]] size_t offset(const uint8_t *ptr) const;

	[[nodiscard]] const std::string &filename() const;

private:
	/**
	 * Calculate the size of the memory mapping from the size of the file
	 * @param fileSize The size of the file
	 * @return The size of the memory mapping
	 */
	inline static size_t calculateMapSizeFromFilesize(size_t fileSize) {
		size_t mapSize = fileSize;
		size_t page_size = pageSize();
		size_t remainder = mapSize % page_size;
		if (remainder != 0) { mapSize += page_size - remainder; }
		return mapSize;
	}

	inline static size_t pageSize() { return sysconf(_SC_PAGESIZE); }

	inline static size_t getFileSizeFromFd(int fd) {
		struct stat st {};
		if (::fstat(fd, &st) == -1) {
			std::string errorMsg =
			    "Failed to fstat file: " + std::string(strerror(errno));
			throw std::runtime_error(errorMsg);
		}
		return st.st_size;
	}

private:
	std::string path_{};      // The path to the file
	size_t fileSize_{};       // The size of the file
	size_t mapSize_;          // The size of the memory mapping
	uint8_t *map_ = nullptr;  // The memory mapped contents of the file
	int fd_;                  // The file descriptor of the file
};

MemoryMappedFile::Impl::Impl(const std::string &path) {
	try {
		path_ = path;
		if (path_.front() != '/') { // Add Full path is its missing
			path_ = fs::getCurrentWorkingDirectory() + "/" + path_;
			} else {
			path_ = path;
		}
		fd_ = ::open(path.c_str(), O_RDONLY);
		if (fd_ == -1) {
			std::string errorMsg =
			    "Failed to open file '" + path +
			    "' for reading: " + std::string(strerror(errno));
			throw std::runtime_error(errorMsg);
		}
		fileSize_ = getFileSizeFromFd(fd_);
		mapSize_ = calculateMapSizeFromFilesize(fileSize_);

		map_ = static_cast<uint8_t *>(
		    ::mmap(nullptr, mapSize_, PROT_READ, MAP_SHARED, fd_, 0));
		if (map_ == MAP_FAILED) {
			throw std::runtime_error("Failed to mmap file '" + path + "'");
		}
	} catch (std::exception &e) {
		safs_pretty_syslog(LOG_ERR, "Failed to map file '%s': %s", path.c_str(),
		                   e.what());
		throw e;
	}
}

MemoryMappedFile::MemoryMappedFile() = default;
MemoryMappedFile::~MemoryMappedFile() = default;

MemoryMappedFile::MemoryMappedFile(const std::string &path)
    : pimpl(std::make_unique<Impl>(path)) {}

MemoryMappedFile::Impl::~Impl() {
	try {
		::munmap(map_, mapSize_);
	} catch (...) {
		safs_pretty_syslog(LOG_ERR, "Failed to unmap file '%s'", path_.c_str());
	}
	if (fd_ != -1) { ::close(fd_); }
}

uint8_t *MemoryMappedFile::Impl::seek(size_t offset) const {
	if (map_ == nullptr) {
		throw std::runtime_error("File is not mapped");
	}
	if (offset >= fileSize_) {
		throw std::out_of_range("Offset is out of bounds");
	}
	return map_ + offset;
}

uint8_t *MemoryMappedFile::seek(size_t offset) const {
	return pimpl->seek(offset);
}

size_t MemoryMappedFile::Impl::read(size_t &offset, uint8_t *buf, size_t size,
                                    bool moveOffset) const {
	if (map_ == nullptr) {
		throw std::runtime_error("File is not mapped");
	}
	auto ptr = map_ + offset;
	auto read_size = std::min(size, fileSize_ - offset);
	memcpy(buf, ptr, read_size);
	if (moveOffset) { offset += read_size; }
	return read_size;
}

size_t MemoryMappedFile::read(size_t &offset, uint8_t *buf, size_t size,
                              bool walk) const {
	return pimpl->read(offset, buf, size, walk);
}

size_t MemoryMappedFile::Impl::offset(const uint8_t *ptr) const {
	if (map_ == nullptr) { throw std::runtime_error("File is not mapped"); }
	if (ptr == nullptr) { throw std::runtime_error("Pointer is null"); }
	if (ptr < map_ || ptr >= map_ + fileSize_) {
		throw std::out_of_range("Pointer is out of bounds");
	}
	return ptr - map_;
}

size_t MemoryMappedFile::offset(const uint8_t *ptr) const {
	return pimpl->offset(ptr);
}

const std::string &MemoryMappedFile::Impl::filename() const { return path_; }

const std::string &MemoryMappedFile::filename() const {
	return pimpl->filename();
}

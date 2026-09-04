/*
   Copyright 2026      Leil Storage OÜ

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"

#include <fcntl.h>
#include <fmt/format.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/string_generator.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include "chunkserver/chunkserver_id.h"
#include "common/cwrap.h"
#include "common/massert.h"
#include "slogger/slogger.h"

namespace chunkserver {

namespace {

constexpr size_t kUuidTextSize = 36;
constexpr size_t kUuidFileSize = kUuidTextSize + 1;

std::optional<ChunkserverId> gChunkserverId;

[[noreturn]] void throwSystemError(const std::string &operation, int errorNumber) {
	throw std::system_error(errorNumber, std::generic_category(), operation);
}

boost::uuids::uuid asBoostUuid(const ChunkserverId &chunkserverId) {
	boost::uuids::uuid uuid;
	std::ranges::copy(chunkserverId, uuid.begin());
	return uuid;
}

ChunkserverId fromBoostUuid(const boost::uuids::uuid &uuid) {
	ChunkserverId chunkserverId{};
	std::ranges::copy(uuid, chunkserverId.begin());
	return chunkserverId;
}

bool isVersion4(const boost::uuids::uuid &uuid) {
	return uuid.version() == boost::uuids::uuid::version_random_number_based &&
	       uuid.variant() == boost::uuids::uuid::variant_rfc_4122;
}

[[noreturn]] void throwNotCanonicalUuid() {
	throw std::invalid_argument("chunkserver id is not a canonical lowercase UUIDv4");
}

void validateGeneratedChunkserverId(const ChunkserverId &chunkserverId) {
	if (!isVersion4(asBoostUuid(chunkserverId))) {
		throw std::runtime_error("chunkserver id generator did not return a UUIDv4");
	}
}

void closeFile(FileDescriptor &file, const std::filesystem::path &path) {
	if (::close(file.release()) != 0) {
		throwSystemError(fmt::format("failed to close '{}'", path.string()), errno);
	}
}

std::optional<ChunkserverId> readChunkserverId(const std::filesystem::path &path) {
	const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (descriptor < 0) {
		if (errno == ENOENT) { return std::nullopt; }
		throwSystemError(fmt::format("failed to open existing chunkserver id '{}'", path.string()),
		                 errno);
	}
	FileDescriptor file(descriptor);

	struct stat status {};
	if (::fstat(file.get(), &status) != 0) {
		throwSystemError(fmt::format("failed to inspect chunkserver id '{}'", path.string()),
		                 errno);
	}
	if (!S_ISREG(status.st_mode) || (status.st_size != static_cast<off_t>(kUuidTextSize) &&
	                                 status.st_size != static_cast<off_t>(kUuidFileSize))) {
		throw std::runtime_error(
		    fmt::format("chunkserver id '{}' is not a canonical UUIDv4 file", path.string()));
	}

	std::array<char, kUuidFileSize> content{};
	size_t offset = 0;
	const size_t expectedSize = static_cast<size_t>(status.st_size);
	while (offset < expectedSize) {
		const ssize_t bytesRead =
		    ::read(file.get(), content.data() + offset, expectedSize - offset);
		if (bytesRead < 0) {
			if (errno == EINTR) { continue; }
			throwSystemError(fmt::format("failed to read chunkserver id '{}'", path.string()),
			                 errno);
		}
		if (bytesRead == 0) {
			throw std::runtime_error(
			    fmt::format("chunkserver id '{}' ended before its expected size", path.string()));
		}
		offset += static_cast<size_t>(bytesRead);
	}
	closeFile(file, path);

	return parseChunkserverId(std::string_view(content.data(), expectedSize));
}

void writeAll(int descriptor, std::string_view content, const std::filesystem::path &path) {
	size_t offset = 0;
	while (offset < content.size()) {
		const ssize_t bytesWritten =
		    ::write(descriptor, content.data() + offset, content.size() - offset);
		if (bytesWritten < 0) {
			if (errno == EINTR) { continue; }
			throwSystemError(fmt::format("failed to write chunkserver id to '{}'", path.string()),
			                 errno);
		}
		if (bytesWritten == 0) {
			throw std::runtime_error(
			    fmt::format("writing chunkserver id to '{}' made no progress", path.string()));
		}
		offset += static_cast<size_t>(bytesWritten);
	}
}

bool directorySyncUnsupported(int errorNumber) {
	return errorNumber == EINVAL || errorNumber == EOPNOTSUPP;
}

void syncParentDirectory(const std::filesystem::path &path) {
	const std::filesystem::path parent = path.parent_path().empty() ? "." : path.parent_path();
	const int descriptor = ::open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
	if (descriptor < 0) {
		throwSystemError(
		    fmt::format("failed to open chunkserver id directory '{}'", parent.string()), errno);
	}
	FileDescriptor directory(descriptor);

	if (::fsync(directory.get()) != 0) {
		const int errorNumber = errno;
		if (directorySyncUnsupported(errorNumber)) {
			safs::log_warn("filesystem does not support syncing chunkserver id directory '{}': {}",
			               parent.string(), errorString(errorNumber));
		} else {
			throwSystemError(
			    fmt::format("failed to sync chunkserver id directory '{}'", parent.string()),
			    errorNumber);
		}
	}
	closeFile(directory, parent);
}

void persistChunkserverId(const std::filesystem::path &path, const ChunkserverId &chunkserverId) {
	const std::filesystem::path temporaryPath = path.string() + ".tmp";
	if (::unlink(temporaryPath.c_str()) != 0 && errno != ENOENT) {
		throwSystemError(
		    fmt::format("failed to remove stale chunkserver id file '{}'", temporaryPath.string()),
		    errno);
	}

	try {
		const int descriptor =
		    ::open(temporaryPath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
		           S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
		if (descriptor < 0) {
			throwSystemError(
			    fmt::format("failed to create chunkserver id file '{}'", temporaryPath.string()),
			    errno);
		}
		FileDescriptor temporaryFile(descriptor);
		const std::string content = formatChunkserverId(chunkserverId) + "\n";

		writeAll(temporaryFile.get(), content, temporaryPath);
		if (::fsync(temporaryFile.get()) != 0) {
			throwSystemError(
			    fmt::format("failed to sync chunkserver id file '{}'", temporaryPath.string()),
			    errno);
		}
		closeFile(temporaryFile, temporaryPath);

		if (::rename(temporaryPath.c_str(), path.c_str()) != 0) {
			throwSystemError(fmt::format("failed to rename chunkserver id file '{}' to '{}'",
			                             temporaryPath.string(), path.string()),
			                 errno);
		}
		syncParentDirectory(path);
	} catch (...) {
		// A failure after the rename leaves the final file valid; this unlink is then a no-op.
		::unlink(temporaryPath.c_str());
		throw;
	}
}

}  // namespace

ChunkserverId parseChunkserverId(std::string_view text) {
	if (text.size() == kUuidFileSize && text.back() == '\n') { text.remove_suffix(1); }
	if (text.size() != kUuidTextSize) { throwNotCanonicalUuid(); }

	boost::uuids::uuid uuid;
	try {
		uuid = boost::uuids::string_generator{}(text.begin(), text.end());
	} catch (const std::runtime_error &) { throwNotCanonicalUuid(); }

	if (boost::uuids::to_string(uuid) != text || !isVersion4(uuid)) { throwNotCanonicalUuid(); }
	return fromBoostUuid(uuid);
}

std::string formatChunkserverId(const ChunkserverId &chunkserverId) {
	return boost::uuids::to_string(asBoostUuid(chunkserverId));
}

ChunkserverId generateChunkserverId() { return fromBoostUuid(boost::uuids::random_generator{}()); }

ChunkserverId loadOrCreateChunkserverId(const std::filesystem::path &path,
                                        const ChunkserverIdGenerator &generator) {
	if (const auto existing = readChunkserverId(path)) { return *existing; }

	const ChunkserverId generated = generator();
	validateGeneratedChunkserverId(generated);
	persistChunkserverId(path, generated);
	return generated;
}

int chunkserverIdInit() {
	try {
		if (gChunkserverId) {
			safs::log_err("chunkserver identity was initialized more than once");
			return -1;
		}

		const ChunkserverId resolved = loadOrCreateChunkserverId(
		    std::filesystem::absolute(kChunkserverIdFilename), generateChunkserverId);
		gChunkserverId = resolved;
		safs::log_info("chunkserver identity: {}", formatChunkserverId(resolved));
		return 0;
	} catch (const std::exception &exception) {
		safs::log_err("failed to initialize chunkserver identity: {}", exception.what());
		return -1;
	}
}

const ChunkserverId &chunkserverId() {
	sassert(gChunkserverId.has_value());
	return *gChunkserverId;
}

}  // namespace chunkserver

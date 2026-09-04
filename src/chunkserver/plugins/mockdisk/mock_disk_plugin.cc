/*
   Copyright 2026 Leil Storage OÜ

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

#include <charconv>
#include <string>

#include "chunkserver-common/disk_plugin.h"
#include "mock_disk.h"
#include "slogger/slogger.h"

namespace mockdisk {

/// Plugin instantiating MockDisk objects for hdd.cfg lines of the form:
///   mock:<chunkCount>:<firstChunkId>:<path>
/// <path> must be a real directory (lock files and trash bookkeeping only).
/// Intended exclusively for tests; never use in production.
class MockDiskPlugin : public DiskPlugin {
public:
	MockDiskPlugin() = default;

	MockDiskPlugin(const MockDiskPlugin &) = delete;
	MockDiskPlugin(MockDiskPlugin &&) = delete;
	MockDiskPlugin &operator=(const MockDiskPlugin &) = delete;
	MockDiskPlugin &operator=(MockDiskPlugin &&) = delete;

	void reload() override {}

	void cleanup() override {}

	std::string name() override { return "MockDiskPlugin"; }

	std::string prefix() override { return "mock"; }

	IDisk *createDisk(const disk::Configuration &configuration) override {
		// The generic parser leaves everything after "mock:" in metaPath
		// (with a trailing '/' appended): "<chunkCount>:<firstChunkId>:<path>/"
		const std::string &spec = configuration.metaPath;

		const auto firstColon = spec.find(':');
		const auto secondColon =
		    firstColon == std::string::npos ? std::string::npos : spec.find(':', firstColon + 1);

		if (secondColon == std::string::npos) {
			safs::log_err(
			    "mock disk: invalid specification '{}', expected "
			    "mock:<chunkCount>:<firstChunkId>:<path>",
			    spec);
			return nullptr;
		}

		uint64_t chunkCount = 0;
		uint64_t firstChunkId = 0;
		const auto countResult = std::from_chars(spec.data(), spec.data() + firstColon, chunkCount);
		const auto idResult =
		    std::from_chars(spec.data() + firstColon + 1, spec.data() + secondColon, firstChunkId);
		const std::string path = spec.substr(secondColon + 1);

		if (countResult.ec != std::errc() || countResult.ptr != spec.data() + firstColon ||
		    idResult.ec != std::errc() || idResult.ptr != spec.data() + secondColon ||
		    chunkCount == 0 || firstChunkId == 0 || path.empty() || path.front() != '/') {
			safs::log_err(
			    "mock disk: invalid specification '{}', expected "
			    "mock:<chunkCount>:<firstChunkId>:<path> with chunkCount > 0, "
			    "firstChunkId > 0 and an absolute path",
			    spec);
			return nullptr;
		}

		const disk::Configuration pathConfiguration(path, path, configuration.isMarkedForRemoval,
		                                            false);

		safs::log_info("mock disk: creating disk at {} with {} fake chunks, first id {}", path,
		               chunkCount, firstChunkId);

		return new MockDisk(pathConfiguration, chunkCount, firstChunkId);
	}

	/// Factory method used by the plugin manager to instantiate this plugin.
	static boost::shared_ptr<MockDiskPlugin> create() {
		return boost::shared_ptr<MockDiskPlugin>(new MockDiskPlugin());
	}
};

}  // namespace mockdisk

BOOST_DLL_ALIAS(mockdisk::MockDiskPlugin::create, createPlugin)

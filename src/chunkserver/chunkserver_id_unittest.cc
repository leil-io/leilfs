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

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "chunkserver/chunkserver_id.h"
#include "unittests/TemporaryDirectory.h"

namespace chunkserver {
namespace {

constexpr std::string_view kValidId = "123e4567-e89b-42d3-a456-426614174000";
constexpr ChunkserverId kValidChunkserverId = {
    0x12, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3, 0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x00,
};

void writeFile(const std::filesystem::path &path, std::string_view content) {
	std::ofstream output(path, std::ios::binary);
	ASSERT_TRUE(output.is_open());
	output.write(content.data(), static_cast<std::streamsize>(content.size()));
	ASSERT_TRUE(output.good());
}

std::string readFile(const std::filesystem::path &path) {
	std::ifstream input(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

ChunkserverIdGenerator fixedGenerator(const ChunkserverId &chunkserverId) {
	return [chunkserverId]() { return chunkserverId; };
}

TEST(ChunkserverIdTests, ParsesAndFormatsCanonicalVersion4) {
	const ChunkserverId parsed = parseChunkserverId(kValidId);

	EXPECT_EQ(formatChunkserverId(parsed), kValidId);
	EXPECT_EQ(parsed[6] & 0xf0, 0x40);
	EXPECT_EQ(parsed[8] & 0xc0, 0x80);
}

TEST(ChunkserverIdTests, AcceptsOneTrailingNewline) {
	EXPECT_EQ(formatChunkserverId(parseChunkserverId(std::string(kValidId) + "\n")), kValidId);
}

TEST(ChunkserverIdTests, RejectsNoncanonicalOrNonVersion4Text) {
	const std::vector<std::string> invalidValues = {
	    "",
	    "00000000-0000-0000-0000-000000000000",
	    "-23e4567-e89b-42d3-a456-426614174000",
	    "123E4567-e89b-42d3-a456-426614174000",
	    "{123e4567-e89b-42d3-a456-426614174000}",
	    "123e4567-e89b-12d3-a456-426614174000",
	    "123e4567-e89b-42d3-7456-426614174000",
	    "123e4567-e89b-42d3-a456-426614174000 ",
	    "123e4567-e89b-42d3-a456-426614174000\n\n",
	    "123e4567-e89b-42d3-a456-42661417400g",
	    "123e4567e-e89b-42d3-a456-426614174000",
	};

	for (const std::string &value : invalidValues) {
		EXPECT_THROW(parseChunkserverId(value), std::invalid_argument) << value;
	}
}

TEST(ChunkserverIdTests, RejectsDashInsideLastGroupOfSlicedView) {
	const std::string longer = "123e4567-e89b-42d3-a456-4266141740-0a";
	const std::string_view sliced(longer.data(), kValidId.size());

	EXPECT_THROW(parseChunkserverId(sliced), std::invalid_argument);
}

TEST(ChunkserverIdTests, GeneratesVersion4AndVariantBits) {
	const ChunkserverId generated = generateChunkserverId();

	EXPECT_EQ(generated[6] & 0xf0, 0x40);
	EXPECT_EQ(generated[8] & 0xc0, 0x80);
	EXPECT_EQ(formatChunkserverId(parseChunkserverId(formatChunkserverId(generated))),
	          formatChunkserverId(generated));
}

TEST(ChunkserverIdTests, RejectsInvalidGeneratedIdentity) {
	TemporaryDirectory temporaryDirectory("/tmp", "chunkserver-id-invalid-generator");
	const std::filesystem::path idPath =
	    std::filesystem::path(temporaryDirectory.name()) / kChunkserverIdFilename;
	ChunkserverId wrongVersion = kValidChunkserverId;
	wrongVersion[6] = static_cast<uint8_t>((wrongVersion[6] & 0x0f) | 0x10);
	ChunkserverId wrongVariant = kValidChunkserverId;
	wrongVariant[8] &= 0x3f;
	const std::vector<ChunkserverId> invalidIds = {ChunkserverId{}, wrongVersion, wrongVariant};

	for (const ChunkserverId &invalidId : invalidIds) {
		EXPECT_THROW(loadOrCreateChunkserverId(idPath, fixedGenerator(invalidId)),
		             std::runtime_error);
	}
	EXPECT_FALSE(std::filesystem::exists(idPath));
	EXPECT_FALSE(std::filesystem::exists(idPath.string() + ".tmp"));
}

TEST(ChunkserverIdTests, PropagatesGeneratorFailure) {
	TemporaryDirectory temporaryDirectory("/tmp", "chunkserver-id-generator-failure");
	const std::filesystem::path idPath =
	    std::filesystem::path(temporaryDirectory.name()) / kChunkserverIdFilename;
	const auto failingGenerator = []() -> ChunkserverId {
		throw std::runtime_error("injected generator failure");
	};

	EXPECT_THROW(loadOrCreateChunkserverId(idPath, failingGenerator), std::runtime_error);
	EXPECT_FALSE(std::filesystem::exists(idPath));
	EXPECT_FALSE(std::filesystem::exists(idPath.string() + ".tmp"));
}

TEST(ChunkserverIdTests, CreatesThenReusesIdentityWithoutRewriting) {
	TemporaryDirectory temporaryDirectory("/tmp", "chunkserver-id-reuse");
	const std::filesystem::path idPath =
	    std::filesystem::path(temporaryDirectory.name()) / kChunkserverIdFilename;
	const ChunkserverId created =
	    loadOrCreateChunkserverId(idPath, fixedGenerator(kValidChunkserverId));
	struct stat beforeStatus {};
	ASSERT_EQ(::stat(idPath.c_str(), &beforeStatus), 0);

	bool generatorCalled = false;
	const ChunkserverId reused = loadOrCreateChunkserverId(idPath, [&generatorCalled]() {
		generatorCalled = true;
		return kValidChunkserverId;
	});
	struct stat afterStatus {};
	ASSERT_EQ(::stat(idPath.c_str(), &afterStatus), 0);

	EXPECT_EQ(reused, created);
	EXPECT_FALSE(generatorCalled);
	EXPECT_EQ(afterStatus.st_ino, beforeStatus.st_ino);
	EXPECT_EQ(readFile(idPath), formatChunkserverId(created) + "\n");
	EXPECT_FALSE(std::filesystem::exists(idPath.string() + ".tmp"));
}

TEST(ChunkserverIdTests, ReplacesStaleTemporaryFile) {
	TemporaryDirectory temporaryDirectory("/tmp", "chunkserver-id-stale-temp");
	const std::filesystem::path idPath =
	    std::filesystem::path(temporaryDirectory.name()) / kChunkserverIdFilename;
	writeFile(idPath.string() + ".tmp", "incomplete");

	const ChunkserverId created =
	    loadOrCreateChunkserverId(idPath, fixedGenerator(kValidChunkserverId));

	EXPECT_EQ(readFile(idPath), formatChunkserverId(created) + "\n");
	EXPECT_FALSE(std::filesystem::exists(idPath.string() + ".tmp"));
}

TEST(ChunkserverIdTests, RejectsInvalidExistingFileWithoutReplacingIt) {
	TemporaryDirectory temporaryDirectory("/tmp", "chunkserver-id-invalid");
	const std::filesystem::path idPath =
	    std::filesystem::path(temporaryDirectory.name()) / kChunkserverIdFilename;
	const std::string invalidContent = "123E4567-e89b-42d3-a456-426614174000\n";
	writeFile(idPath, invalidContent);
	bool generatorCalled = false;

	EXPECT_THROW(loadOrCreateChunkserverId(idPath,
	                                       [&generatorCalled]() {
		                                       generatorCalled = true;
		                                       return kValidChunkserverId;
	                                       }),
	             std::invalid_argument);
	EXPECT_FALSE(generatorCalled);
	EXPECT_EQ(readFile(idPath), invalidContent);
}

TEST(ChunkserverIdTests, AcceptsExistingFileWithoutTrailingNewline) {
	TemporaryDirectory temporaryDirectory("/tmp", "chunkserver-id-no-newline");
	const std::filesystem::path idPath =
	    std::filesystem::path(temporaryDirectory.name()) / kChunkserverIdFilename;
	writeFile(idPath, kValidId);
	bool generatorCalled = false;

	const ChunkserverId loaded = loadOrCreateChunkserverId(idPath, [&generatorCalled]() {
		generatorCalled = true;
		return kValidChunkserverId;
	});

	EXPECT_EQ(formatChunkserverId(loaded), kValidId);
	EXPECT_FALSE(generatorCalled);
	EXPECT_EQ(readFile(idPath), kValidId);
}

TEST(ChunkserverIdTests, RejectsExistingFileOfUnexpectedSize) {
	TemporaryDirectory temporaryDirectory("/tmp", "chunkserver-id-wrong-size");
	const std::filesystem::path idPath =
	    std::filesystem::path(temporaryDirectory.name()) / kChunkserverIdFilename;
	const std::vector<std::string> contents = {"", std::string(kValidId) + "\n\n"};

	for (const std::string &content : contents) {
		writeFile(idPath, content);
		EXPECT_THROW(loadOrCreateChunkserverId(idPath, fixedGenerator(kValidChunkserverId)),
		             std::runtime_error)
		    << content.size();
		EXPECT_EQ(readFile(idPath), content);
	}
}

TEST(ChunkserverIdTests, RejectsNonRegularExistingPath) {
	TemporaryDirectory temporaryDirectory("/tmp", "chunkserver-id-directory");
	const std::filesystem::path idPath =
	    std::filesystem::path(temporaryDirectory.name()) / kChunkserverIdFilename;
	ASSERT_TRUE(std::filesystem::create_directory(idPath));

	EXPECT_THROW(loadOrCreateChunkserverId(idPath, fixedGenerator(kValidChunkserverId)),
	             std::runtime_error);
	EXPECT_TRUE(std::filesystem::is_directory(idPath));
}

TEST(ChunkserverIdTests, RejectsDanglingSymlinkWithoutReplacingIt) {
	TemporaryDirectory temporaryDirectory("/tmp", "chunkserver-id-symlink");
	const std::filesystem::path idPath =
	    std::filesystem::path(temporaryDirectory.name()) / kChunkserverIdFilename;
	std::filesystem::create_symlink("missing-target", idPath);
	ASSERT_TRUE(std::filesystem::is_symlink(idPath));
	bool generatorCalled = false;

	EXPECT_THROW(loadOrCreateChunkserverId(idPath,
	                                       [&generatorCalled]() {
		                                       generatorCalled = true;
		                                       return kValidChunkserverId;
	                                       }),
	             std::system_error);
	EXPECT_FALSE(generatorCalled);
	EXPECT_TRUE(std::filesystem::is_symlink(idPath));
}

TEST(ChunkserverIdTests, RejectsFifoWithoutBlocking) {
	TemporaryDirectory temporaryDirectory("/tmp", "chunkserver-id-fifo");
	const std::filesystem::path idPath =
	    std::filesystem::path(temporaryDirectory.name()) / kChunkserverIdFilename;
	ASSERT_EQ(::mkfifo(idPath.c_str(), S_IRUSR | S_IWUSR), 0);

	EXPECT_THROW(loadOrCreateChunkserverId(idPath, fixedGenerator(kValidChunkserverId)),
	             std::runtime_error);
}

TEST(ChunkserverIdTests, FailsIfStaleTemporaryPathCannotBeRemoved) {
	TemporaryDirectory temporaryDirectory("/tmp", "chunkserver-id-blocked-temp");
	const std::filesystem::path idPath =
	    std::filesystem::path(temporaryDirectory.name()) / kChunkserverIdFilename;
	const std::filesystem::path temporaryPath = idPath.string() + ".tmp";
	ASSERT_TRUE(std::filesystem::create_directory(temporaryPath));

	EXPECT_THROW(loadOrCreateChunkserverId(idPath, fixedGenerator(kValidChunkserverId)),
	             std::system_error);
	EXPECT_FALSE(std::filesystem::exists(idPath));
	EXPECT_TRUE(std::filesystem::is_directory(temporaryPath));
}

}  // namespace
}  // namespace chunkserver

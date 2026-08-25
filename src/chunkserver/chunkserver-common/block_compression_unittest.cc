/*
   Copyright 2026 Leil Storage

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
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "chunkserver-common/block_compression.h"

using block_compression::Algorithm;

namespace {

/// Every algorithm that actually compresses. Each test that is about the
/// wrapper's contract rather than about one library runs over all of them, so
/// adding an algorithm cannot quietly skip the contract.
const std::vector<Algorithm> &compressingAlgorithms() {
	static const std::vector<Algorithm> algorithms = {Algorithm::Zstd, Algorithm::Lz4};
	return algorithms;
}

/// A block that compresses well: short repeating text, like the payloads the
/// SMR compression tests write.
std::vector<uint8_t> compressibleBlock() {
	const std::string pattern = "the quick brown fox jumps over the lazy dog 0123456789 ";
	std::vector<uint8_t> block(SFSBLOCKSIZE);
	for (size_t i = 0; i < block.size(); ++i) {
		block[i] = static_cast<uint8_t>(pattern[i % pattern.size()]);
	}
	return block;
}

/// A block no algorithm can shrink. Seeded, so a failure reproduces.
std::vector<uint8_t> incompressibleBlock() {
	std::mt19937 generator(20260825);
	std::uniform_int_distribution<int> byte(0, 255);
	std::vector<uint8_t> block(SFSBLOCKSIZE);
	for (auto &value : block) { value = static_cast<uint8_t>(byte(generator)); }
	return block;
}

/// The dictionary the disk plugins build: a raw sample of the first block.
std::vector<uint8_t> sampleDictionary(const std::vector<uint8_t> &block, size_t size) {
	return std::vector<uint8_t>(block.begin(), block.begin() + size);
}

constexpr int kZstdLevel = 3;
/// What the write paths pass: a block that needs the whole SFSBLOCKSIZE is
/// stored raw instead, so compression is never given room for more.
constexpr size_t kWriteDstCapacity = SFSBLOCKSIZE - 1;

}  // namespace

TEST(BlockCompressionTests, EveryAlgorithmRoundTripsABlockWithoutADictionary) {
	const std::vector<uint8_t> source = compressibleBlock();

	for (const Algorithm algorithm : compressingAlgorithms()) {
		SCOPED_TRACE(block_compression::algorithmName(algorithm));

		std::vector<uint8_t> compressed(kWriteDstCapacity);
		const ssize_t compressedSize =
		    block_compression::compressBlock(algorithm, nullptr, source.data(), source.size(),
		                                     compressed.data(), compressed.size(), kZstdLevel);
		ASSERT_GT(compressedSize, 0) << "compressible data must compress";
		ASSERT_LT(static_cast<size_t>(compressedSize), source.size());

		std::vector<uint8_t> decompressed(SFSBLOCKSIZE);
		ASSERT_EQ(static_cast<ssize_t>(SFSBLOCKSIZE),
		          block_compression::decompressBlock(algorithm, nullptr, compressed.data(),
		                                             compressedSize, decompressed.data(),
		                                             decompressed.size()));
		EXPECT_EQ(source, decompressed);
	}
}

TEST(BlockCompressionTests, EveryAlgorithmRoundTripsABlockWithADictionary) {
	const std::vector<uint8_t> source = compressibleBlock();
	const std::vector<uint8_t> dictionaryBytes = sampleDictionary(source, 16380);

	for (const Algorithm algorithm : compressingAlgorithms()) {
		SCOPED_TRACE(block_compression::algorithmName(algorithm));

		auto compressDict = block_compression::createCompressDict(
		    algorithm, dictionaryBytes.data(), dictionaryBytes.size(), kZstdLevel);
		auto decompressDict = block_compression::createDecompressDict(
		    algorithm, dictionaryBytes.data(), dictionaryBytes.size());
		ASSERT_NE(nullptr, compressDict);
		ASSERT_NE(nullptr, decompressDict);

		std::vector<uint8_t> compressed(kWriteDstCapacity);
		const ssize_t compressedSize = block_compression::compressBlock(
		    algorithm, compressDict.get(), source.data(), source.size(), compressed.data(),
		    compressed.size(), kZstdLevel);
		ASSERT_GT(compressedSize, 0);

		std::vector<uint8_t> decompressed(SFSBLOCKSIZE);
		ASSERT_EQ(static_cast<ssize_t>(SFSBLOCKSIZE),
		          block_compression::decompressBlock(algorithm, decompressDict.get(),
		                                             compressed.data(), compressedSize,
		                                             decompressed.data(), decompressed.size()));
		EXPECT_EQ(source, decompressed);
	}
}

// A dictionary is prepared once and used for every block of a chunk, so it has
// to keep working - and keep producing the same bytes - across calls.
TEST(BlockCompressionTests, OneDictionaryServesRepeatedBlocks) {
	const std::vector<uint8_t> source = compressibleBlock();
	const std::vector<uint8_t> dictionaryBytes = sampleDictionary(source, 4096);

	for (const Algorithm algorithm : compressingAlgorithms()) {
		SCOPED_TRACE(block_compression::algorithmName(algorithm));

		auto compressDict = block_compression::createCompressDict(
		    algorithm, dictionaryBytes.data(), dictionaryBytes.size(), kZstdLevel);
		auto decompressDict = block_compression::createDecompressDict(
		    algorithm, dictionaryBytes.data(), dictionaryBytes.size());
		ASSERT_NE(nullptr, compressDict);
		ASSERT_NE(nullptr, decompressDict);

		std::vector<uint8_t> firstCompressed(kWriteDstCapacity);
		const ssize_t firstSize = block_compression::compressBlock(
		    algorithm, compressDict.get(), source.data(), source.size(), firstCompressed.data(),
		    firstCompressed.size(), kZstdLevel);
		ASSERT_GT(firstSize, 0);

		for (int repetition = 0; repetition < 3; ++repetition) {
			std::vector<uint8_t> compressed(kWriteDstCapacity);
			const ssize_t compressedSize = block_compression::compressBlock(
			    algorithm, compressDict.get(), source.data(), source.size(), compressed.data(),
			    compressed.size(), kZstdLevel);
			ASSERT_EQ(firstSize, compressedSize) << "repetition " << repetition;

			std::vector<uint8_t> decompressed(SFSBLOCKSIZE);
			ASSERT_EQ(static_cast<ssize_t>(SFSBLOCKSIZE),
			          block_compression::decompressBlock(algorithm, decompressDict.get(),
			                                             compressed.data(), compressedSize,
			                                             decompressed.data(),
			                                             decompressed.size()));
			EXPECT_EQ(source, decompressed) << "repetition " << repetition;
		}
	}
}

// The write path caps the output at SFSBLOCKSIZE - 1 and stores the block raw
// when compression does not answer with a positive size. Whether the algorithm
// reports failure or "does not fit" must not matter to that caller.
TEST(BlockCompressionTests, AnIncompressibleBlockIsNotCompressed) {
	const std::vector<uint8_t> source = incompressibleBlock();

	for (const Algorithm algorithm : compressingAlgorithms()) {
		SCOPED_TRACE(block_compression::algorithmName(algorithm));

		std::vector<uint8_t> compressed(kWriteDstCapacity);
		EXPECT_LE(block_compression::compressBlock(algorithm, nullptr, source.data(),
		                                           source.size(), compressed.data(),
		                                           compressed.size(), kZstdLevel),
		          0);
	}
}

// The two dictionary arms are not interchangeable, and using the wrong one
// would produce blocks no reader could decode. Refusing makes the caller store
// the block raw instead, which every reader can.
TEST(BlockCompressionTests, ADictionaryOfAnotherAlgorithmIsRefused) {
	const std::vector<uint8_t> source = compressibleBlock();
	const std::vector<uint8_t> dictionaryBytes = sampleDictionary(source, 4096);

	auto zstdCompressDict = block_compression::createCompressDict(
	    Algorithm::Zstd, dictionaryBytes.data(), dictionaryBytes.size(), kZstdLevel);
	auto zstdDecompressDict = block_compression::createDecompressDict(
	    Algorithm::Zstd, dictionaryBytes.data(), dictionaryBytes.size());
	ASSERT_NE(nullptr, zstdCompressDict);
	ASSERT_NE(nullptr, zstdDecompressDict);

	std::vector<uint8_t> compressed(kWriteDstCapacity);
	EXPECT_LE(block_compression::compressBlock(Algorithm::Lz4, zstdCompressDict.get(),
	                                           source.data(), source.size(), compressed.data(),
	                                           compressed.size(), kZstdLevel),
	          0);

	std::vector<uint8_t> decompressed(SFSBLOCKSIZE);
	EXPECT_LT(block_compression::decompressBlock(Algorithm::Lz4, zstdDecompressDict.get(),
	                                             compressed.data(), compressed.size(),
	                                             decompressed.data(), decompressed.size()),
	          0);
}

// Algorithm::None never compresses: it is what a chunk in the legacy format
// carries, and the compressed paths must not be reachable with it.
TEST(BlockCompressionTests, NoneCompressesNothing) {
	const std::vector<uint8_t> source = compressibleBlock();
	const std::vector<uint8_t> dictionaryBytes = sampleDictionary(source, 4096);

	EXPECT_EQ(nullptr, block_compression::createCompressDict(Algorithm::None,
	                                                         dictionaryBytes.data(),
	                                                         dictionaryBytes.size(), kZstdLevel));
	EXPECT_EQ(nullptr, block_compression::createDecompressDict(
	                       Algorithm::None, dictionaryBytes.data(), dictionaryBytes.size()));

	std::vector<uint8_t> compressed(kWriteDstCapacity);
	EXPECT_LE(block_compression::compressBlock(Algorithm::None, nullptr, source.data(),
	                                           source.size(), compressed.data(),
	                                           compressed.size(), kZstdLevel),
	          0);
	EXPECT_EQ(0U, block_compression::compressBound(Algorithm::None, SFSBLOCKSIZE));
}

// An empty dictionary means "this chunk has none", not "prepare an empty one".
TEST(BlockCompressionTests, AnEmptyDictionaryPreparesNothing) {
	const std::vector<uint8_t> empty;

	for (const Algorithm algorithm : compressingAlgorithms()) {
		SCOPED_TRACE(block_compression::algorithmName(algorithm));

		EXPECT_EQ(nullptr,
		          block_compression::createCompressDict(algorithm, empty.data(), 0, kZstdLevel));
		EXPECT_EQ(nullptr, block_compression::createDecompressDict(algorithm, empty.data(), 0));
	}
}

TEST(BlockCompressionTests, AlgorithmNamesAreTheConfigSpellings) {
	EXPECT_EQ(Algorithm::None, block_compression::algorithmFromName("none"));
	EXPECT_EQ(Algorithm::Zstd, block_compression::algorithmFromName("zstd"));
	EXPECT_EQ(Algorithm::Lz4, block_compression::algorithmFromName("lz4"));

	// Config values are typed by hand, so the spelling is case insensitive.
	EXPECT_EQ(Algorithm::Zstd, block_compression::algorithmFromName("ZSTD"));
	EXPECT_EQ(Algorithm::Lz4, block_compression::algorithmFromName("LZ4"));

	EXPECT_FALSE(block_compression::algorithmFromName("").has_value());
	EXPECT_FALSE(block_compression::algorithmFromName("lz4hc").has_value());
	EXPECT_FALSE(block_compression::algorithmFromName("gzip").has_value());

	// Every name round-trips, so a value reported back is one that parses.
	for (const Algorithm algorithm : {Algorithm::None, Algorithm::Zstd, Algorithm::Lz4}) {
		EXPECT_EQ(algorithm,
		          block_compression::algorithmFromName(block_compression::algorithmName(algorithm)));
	}
}

TEST(BlockCompressionTests, CompressBoundLeavesRoomForFraming) {
	for (const Algorithm algorithm : compressingAlgorithms()) {
		SCOPED_TRACE(block_compression::algorithmName(algorithm));
		EXPECT_GT(block_compression::compressBound(algorithm, SFSBLOCKSIZE),
		          static_cast<size_t>(SFSBLOCKSIZE));
	}
}

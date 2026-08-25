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
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "chunkserver-common/block_compression.h"

using block_compression::Algorithm;

namespace {

/// Every algorithm that compresses. Contract tests run over all of them, so a
/// new algorithm cannot quietly skip the contract.
const std::vector<Algorithm> &compressingAlgorithms() {
	static const std::vector<Algorithm> algorithms = {Algorithm::Zstd, Algorithm::Lz4};
	return algorithms;
}

/// Compresses well: short repeating text, like the SMR tests write.
std::vector<uint8_t> compressibleBlock() {
	const std::string pattern = "the quick brown fox jumps over the lazy dog 0123456789 ";
	std::vector<uint8_t> block(SFSBLOCKSIZE);
	for (size_t i = 0; i < block.size(); ++i) {
		block[i] = static_cast<uint8_t>(pattern[i % pattern.size()]);
	}
	return block;
}

/// Matches worth finding but no trivial structure, so an effort comparison has
/// something to compare. Seeded, so a failure reproduces.
std::vector<uint8_t> moderatelyCompressibleBlock() {
	std::mt19937 generator(20260826);
	std::uniform_int_distribution<uint64_t> token(0, 63);
	std::vector<uint8_t> block(SFSBLOCKSIZE);
	for (size_t i = 0; i + sizeof(uint64_t) <= block.size(); i += sizeof(uint64_t)) {
		const uint64_t value = 0x0123456789ABCDEFULL * token(generator);
		std::memcpy(block.data() + i, &value, sizeof(value));
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
/// What the write paths pass: a block needing the whole SFSBLOCKSIZE is stored
/// raw instead.
constexpr size_t kWriteDstCapacity = SFSBLOCKSIZE - 1;

/// The bytes a write path would store, or empty when it would store the block
/// raw.
std::vector<uint8_t> compressWith(Algorithm algorithm, const block_compression::CompressDict *dict,
                                  const std::vector<uint8_t> &source) {
	std::vector<uint8_t> compressed(kWriteDstCapacity);
	const ssize_t compressedSize =
	    block_compression::compressBlock(algorithm, dict, source.data(), source.size(),
	                                     compressed.data(), compressed.size(), kZstdLevel);
	if (compressedSize <= 0) { return {}; }
	compressed.resize(static_cast<size_t>(compressedSize));
	return compressed;
}

/// Fails unless @p compressed decodes back to @p expected under @p dict.
void expectDecodesTo(Algorithm algorithm, const block_compression::DecompressDict *dict,
                     const std::vector<uint8_t> &compressed,
                     const std::vector<uint8_t> &expected) {
	std::vector<uint8_t> decompressed(SFSBLOCKSIZE);
	ASSERT_EQ(static_cast<ssize_t>(SFSBLOCKSIZE),
	          block_compression::decompressBlock(algorithm, dict, compressed.data(),
	                                             compressed.size(), decompressed.data(),
	                                             decompressed.size()));
	EXPECT_EQ(expected, decompressed);
}

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

// A dictionary is prepared once and reused for every block of a chunk.
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

// Chunks written by one thread share a thread-local compressor state. A block
// referencing what the previous one left there would decode to garbage.
TEST(BlockCompressionTests, InterleavedChunksDoNotShareCompressionHistory) {
	// Two chunks, each with the dictionary the plugins sample from their own
	// first block, so the dictionary genuinely describes its own chunk.
	const std::vector<uint8_t> firstSource = compressibleBlock();
	const std::vector<uint8_t> secondSource = moderatelyCompressibleBlock();
	const std::vector<uint8_t> firstDictBytes = sampleDictionary(firstSource, 4096);
	const std::vector<uint8_t> secondDictBytes = sampleDictionary(secondSource, 4096);

	for (const Algorithm algorithm : compressingAlgorithms()) {
		SCOPED_TRACE(block_compression::algorithmName(algorithm));

		auto firstCompressDict = block_compression::createCompressDict(
		    algorithm, firstDictBytes.data(), firstDictBytes.size(), kZstdLevel);
		auto secondCompressDict = block_compression::createCompressDict(
		    algorithm, secondDictBytes.data(), secondDictBytes.size(), kZstdLevel);
		auto firstDecompressDict = block_compression::createDecompressDict(
		    algorithm, firstDictBytes.data(), firstDictBytes.size());
		auto secondDecompressDict = block_compression::createDecompressDict(
		    algorithm, secondDictBytes.data(), secondDictBytes.size());
		ASSERT_NE(nullptr, firstCompressDict);
		ASSERT_NE(nullptr, secondCompressDict);
		ASSERT_NE(nullptr, firstDecompressDict);
		ASSERT_NE(nullptr, secondDecompressDict);

		// What each chunk's block looks like with nothing else in flight.
		const std::vector<uint8_t> firstAlone =
		    compressWith(algorithm, firstCompressDict.get(), firstSource);
		const std::vector<uint8_t> secondAlone =
		    compressWith(algorithm, secondCompressDict.get(), secondSource);
		ASSERT_FALSE(firstAlone.empty());
		ASSERT_FALSE(secondAlone.empty());

		// The same two blocks, now alternating through the shared state.
		for (int round = 0; round < 3; ++round) {
			SCOPED_TRACE(round);

			const std::vector<uint8_t> firstInterleaved =
			    compressWith(algorithm, firstCompressDict.get(), firstSource);
			EXPECT_EQ(firstAlone, firstInterleaved)
			    << "a chunk's block changed because another chunk's block preceded it";
			expectDecodesTo(algorithm, firstDecompressDict.get(), firstInterleaved, firstSource);

			const std::vector<uint8_t> secondInterleaved =
			    compressWith(algorithm, secondCompressDict.get(), secondSource);
			EXPECT_EQ(secondAlone, secondInterleaved)
			    << "a chunk's block changed because another chunk's block preceded it";
			expectDecodesTo(algorithm, secondDecompressDict.get(), secondInterleaved,
			                secondSource);
		}
	}
}

// The same isolation across both paths: with a dictionary and without touch
// that shared state differently.
TEST(BlockCompressionTests, ADictionaryChunkInterleavesWithADictionarylessOne) {
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

		const std::vector<uint8_t> withDictAlone =
		    compressWith(algorithm, compressDict.get(), source);
		const std::vector<uint8_t> withoutDictAlone = compressWith(algorithm, nullptr, source);
		ASSERT_FALSE(withDictAlone.empty());
		ASSERT_FALSE(withoutDictAlone.empty());

		for (int round = 0; round < 3; ++round) {
			SCOPED_TRACE(round);

			const std::vector<uint8_t> withDict =
			    compressWith(algorithm, compressDict.get(), source);
			EXPECT_EQ(withDictAlone, withDict);
			expectDecodesTo(algorithm, decompressDict.get(), withDict, source);

			const std::vector<uint8_t> withoutDict = compressWith(algorithm, nullptr, source);
			EXPECT_EQ(withoutDictAlone, withoutDict);
			expectDecodesTo(algorithm, nullptr, withoutDict, source);
		}
	}
}

// lz4.h leaves the stream's state undefined when a call fails, and one stream
// serves every chunk a thread writes. Both entry points must reset it, or the
// block after a failed one references history no reader can resolve.
TEST(BlockCompressionTests, Lz4RecoversTheSharedStreamAfterAFailedBlock) {
	const std::vector<uint8_t> source = compressibleBlock();
	const std::vector<uint8_t> dictionaryBytes = sampleDictionary(source, 4096);
	// Fails the compressor: it does not fit what a write path offers.
	const std::vector<uint8_t> unshrinkable = incompressibleBlock();

	auto compressDict = block_compression::createCompressDict(
	    Algorithm::Lz4, dictionaryBytes.data(), dictionaryBytes.size(), kZstdLevel);
	auto decompressDict = block_compression::createDecompressDict(
	    Algorithm::Lz4, dictionaryBytes.data(), dictionaryBytes.size());
	ASSERT_NE(nullptr, compressDict);
	ASSERT_NE(nullptr, decompressDict);

	const std::vector<uint8_t> withDictAlone =
	    compressWith(Algorithm::Lz4, compressDict.get(), source);
	const std::vector<uint8_t> withoutDictAlone = compressWith(Algorithm::Lz4, nullptr, source);
	ASSERT_FALSE(withDictAlone.empty());
	ASSERT_FALSE(withoutDictAlone.empty());

	// Either entry point can be the one that fails, and either can be the one
	// that follows: LZ4_compress_fast_continue() carries state, and
	// LZ4_compress_fast_extState() shares the stream it initializes.
	for (const bool failWithDictionary : {true, false}) {
		SCOPED_TRACE(failWithDictionary ? "failed with a dictionary" : "failed without one");
		const block_compression::CompressDict *failingDict =
		    failWithDictionary ? compressDict.get() : nullptr;

		ASSERT_TRUE(compressWith(Algorithm::Lz4, failingDict, unshrinkable).empty())
		    << "the block meant to fail compressed instead";
		const std::vector<uint8_t> withDictAfter =
		    compressWith(Algorithm::Lz4, compressDict.get(), source);
		EXPECT_EQ(withDictAlone, withDictAfter) << "a failed block left state behind";
		expectDecodesTo(Algorithm::Lz4, decompressDict.get(), withDictAfter, source);

		ASSERT_TRUE(compressWith(Algorithm::Lz4, failingDict, unshrinkable).empty())
		    << "the block meant to fail compressed instead";
		const std::vector<uint8_t> withoutDictAfter =
		    compressWith(Algorithm::Lz4, nullptr, source);
		EXPECT_EQ(withoutDictAlone, withoutDictAfter) << "a failed block left state behind";
		expectDecodesTo(Algorithm::Lz4, nullptr, withoutDictAfter, source);
	}
}

// The write path stores the block raw on any non-positive size, so "failed"
// and "did not fit" must not need telling apart.
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

// The wrong arm would produce blocks no reader could decode; refusing makes
// the caller store the block raw, which every reader can.
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

// None is what a legacy-format chunk carries; the compressed paths must not be
// reachable with it.
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

// Pinned here rather than rediscovered per plugin: getting it wrong costs
// write CPU for a dictionary the algorithm cannot apply cheaply.
TEST(BlockCompressionTests, OnlyZstdAsksForADictionary) {
	EXPECT_TRUE(block_compression::usesDictionary(Algorithm::Zstd));
	EXPECT_FALSE(block_compression::usesDictionary(Algorithm::Lz4));
	EXPECT_FALSE(block_compression::usesDictionary(Algorithm::None));
}

// An algorithm that wants no dictionary must still round-trip without one.
TEST(BlockCompressionTests, ABlockCompressedWithoutADictionaryDecodesAlone) {
	const std::vector<uint8_t> source = compressibleBlock();

	for (const Algorithm algorithm : compressingAlgorithms()) {
		SCOPED_TRACE(block_compression::algorithmName(algorithm));

		std::vector<uint8_t> compressed(kWriteDstCapacity);
		const ssize_t compressedSize =
		    block_compression::compressBlock(algorithm, nullptr, source.data(), source.size(),
		                                     compressed.data(), compressed.size(), kZstdLevel);
		ASSERT_GT(compressedSize, 0);

		std::vector<uint8_t> decompressed(SFSBLOCKSIZE);
		ASSERT_EQ(static_cast<ssize_t>(SFSBLOCKSIZE),
		          block_compression::decompressBlock(algorithm, nullptr, compressed.data(),
		                                             compressedSize, decompressed.data(),
		                                             decompressed.size()));
		EXPECT_EQ(source, decompressed);
	}
}

TEST(BlockCompressionTests, CompressBoundLeavesRoomForFraming) {
	for (const Algorithm algorithm : compressingAlgorithms()) {
		SCOPED_TRACE(block_compression::algorithmName(algorithm));
		EXPECT_GT(block_compression::compressBound(algorithm, SFSBLOCKSIZE),
		          static_cast<size_t>(SFSBLOCKSIZE));
	}
}

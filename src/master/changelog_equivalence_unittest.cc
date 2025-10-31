#include "common/platform.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "kv/itransaction.h"
#include "kv/kv_utils.h"
#include "master/changelog.h"
#include "master/changelog_fdb.h"
#include "master/filesystem.h"
#include "master/filesystem_metadata.h"
#include "master/kv_common_keys.h"
#include "slogger/slogger.h"

using namespace std::string_literals;

namespace {

// Helper: write a simple changelog file with provided lines
static std::string writeTempChangelog(const std::vector<std::string> &lines) {
	// Write the changelog into the current working directory with the expected name
	const std::string path = "changelog.sfs";
	std::ofstream out(path, std::ios::out | std::ios::trunc);
	for (const auto &l : lines) {
		out << l << '\n';
	}
	out.close();
	return path;
}

// Prepare a deterministic log payload for a SESSION entry.
static std::string makeSessionPayload(uint32_t ts, uint32_t sessionId) {
	// File changelog format: <version>: <ts>|SESSION():<sessionId>
	// For FDB we store only the payload part (without "<version>: ")
	return std::to_string(ts) + "|SESSION():" + std::to_string(sessionId);
}

// Make a full file line with version prefix
static std::string makeFileLine(uint64_t version, const std::string &payload) {
	return std::to_string(version) + ": " + payload;
}

// Simple in-memory KV engine for unit testing (no external deps)
namespace memkv {

struct KeyLess {
	bool operator()(const kv::Key &a, const kv::Key &b) const {
		return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end());
	}
};

class Transaction : public kv::IReadWriteTransaction {
public:
	explicit Transaction(std::shared_ptr<std::map<kv::Key, kv::Value, KeyLess>> store)
	    : store_(std::move(store)) {}

	std::optional<kv::Value> get(const kv::Key &key) override {
		auto it = store_->find(key);
		if (it == store_->end()) return std::nullopt;
		return it->second;
	}

	kv::GetRangeResult getRange(const kv::KeySelector &start, const kv::KeySelector &end,
	                            int limit = kv::kDefaultGetRangeLimit) override {
		std::vector<kv::KeyValuePair> out;
		int count = 0;
		auto it = store_->lower_bound(start.getKey());
		// If start is exclusive, advance if equal
		if (!start.isInclusive() && it != store_->end() && it->first == start.getKey()) { ++it; }
		for (; it != store_->end(); ++it) {
			const kv::Key &k = it->first;
			bool pastEnd = end.isInclusive() ? (k > end.getKey()) : (k >= end.getKey());
			if (pastEnd) break;
			out.push_back({k, it->second});
			if (++count >= limit) break;
		}
		bool hasMore = false;
		return kv::GetRangeResult(std::move(out), hasMore);
	}

	void set(const kv::Key &key, const kv::Value &value) override { (*store_)[key] = value; }

	void atomicAdd(const kv::Key &key, const kv::Value &delta) override {
		// Interpret value as little-endian integer and add
		uint64_t cur = 0;
		auto it = store_->find(key);
		if (it != store_->end()) {
			// read little-endian into cur
			for (size_t i = 0; i < it->second.size(); ++i) cur |= (uint64_t)it->second[i] << (8 * i);
		}
		uint64_t d = 0;
		for (size_t i = 0; i < delta.size(); ++i) d |= (uint64_t)delta[i] << (8 * i);
		uint64_t sum = cur + d;
		kv::Value v(sizeof(uint64_t));
		for (size_t i = 0; i < sizeof(uint64_t); ++i) v[i] = (sum >> (8 * i)) & 0xFF;
		(*store_)[key] = std::move(v);
	}

	void remove(const kv::Key &key) override { store_->erase(key); }

	bool commit() override { committed_ = true; return true; }
	std::optional<int64_t> getCommittedVersion() const override { return std::nullopt; }

private:
	std::shared_ptr<std::map<kv::Key, kv::Value, KeyLess>> store_;
	bool committed_{false};
};

class Engine : public kv::IKVEngine {
public:
	Engine() : store_(std::make_shared<std::map<kv::Key, kv::Value, KeyLess>>()) {}
	std::unique_ptr<kv::IReadOnlyTransaction> createReadOnlyTransaction() override {
		return createReadWriteTransaction();
	}
	std::unique_ptr<kv::IReadWriteTransaction> createReadWriteTransaction() override {
		return std::make_unique<Transaction>(store_);
	}
	std::shared_ptr<std::map<kv::Key, kv::Value, KeyLess>> store() { return store_; }
private:
	std::shared_ptr<std::map<kv::Key, kv::Value, KeyLess>> store_;
};

} // namespace memkv

// Insert changelog entries into KV under CHLG_ prefix (engine-agnostic)
static bool seedKVChangelog(kv::IKVEngine &engine, const std::vector<std::pair<uint64_t, std::string>> &entries) {
	auto tr = engine.createReadWriteTransaction();
	if (!tr) { return false; }
	for (const auto &e : entries) {
		const kv::Key key = kv::encodeKeyBE(kChangelogPrefix, static_cast<uint64_t>(e.first));
		const kv::Value val = kv::toBytes(e.second);
		tr->set(key, val);
	}
	return tr->commit();
}

class ChangelogEquivalenceTest : public ::testing::Test {
protected:
	void SetUp() override {
		// Prepare isolated temporary metadata directory and switch CWD
		prevCwd_ = std::filesystem::current_path();
		tempDir_ = std::filesystem::temp_directory_path() / ("sfs_meta_" + std::to_string(::getpid()));
		std::error_code ec;
		std::filesystem::create_directories(tempDir_, ec);

		// Copy pristine metadata template into tempDir as metadata.sfs
		const std::filesystem::path sourceRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
		const std::filesystem::path srcMetadata = sourceRoot / "data" / "metadata.sfs";
		ASSERT_TRUE(std::filesystem::exists(srcMetadata)) << "Missing src/data/metadata.sfs";
		const std::filesystem::path dstMetadata = tempDir_ / "metadata.sfs";
		std::filesystem::copy_file(srcMetadata, dstMetadata, std::filesystem::copy_options::overwrite_existing, ec);
		ASSERT_FALSE(ec) << "Failed to copy metadata template: " << ec.message();

		std::filesystem::current_path(tempDir_);

		// Minimal initialization for metadata
		int rc = fs_init(true);
		ASSERT_EQ(rc, 0) << "fs_init(true) failed";
		baseVersion_ = fs_getversion();
	}

	void TearDown() override {
		// Unload and restore CWD, cleanup temp dir
		fs_unload();
		std::error_code ec;
		std::filesystem::current_path(prevCwd_, ec);
		std::filesystem::remove_all(tempDir_, ec);
	}

	uint64_t baseVersion_{};
	std::filesystem::path prevCwd_;
	std::filesystem::path tempDir_;
};

TEST_F(ChangelogEquivalenceTest, FileVsFDB_SessionEntriesProduceSameVersion) {
	// Prepare deterministic entries. First entry must match current fs version.
	const uint64_t v1 = baseVersion_;
	const uint64_t v2 = baseVersion_ + 1;
	const uint32_t nextSess = gMetadata->nextSessionId().getValue();
	const std::string p1 = makeSessionPayload(/*ts=*/1000u, /*sessionId=*/nextSess);
	const std::string p2 = makeSessionPayload(/*ts=*/1001u, /*sessionId=*/nextSess + 1);

	// 1) Apply via file-based changelog
	const std::string file1 = makeFileLine(v1, p1);
	const std::string file2 = makeFileLine(v2, p2);
	const std::string path = writeTempChangelog({file1, file2});

	load_changelog(path);
	const uint64_t fileVersion = fs_getversion();
	// clean up temporary file
	std::error_code ec;
	std::filesystem::remove(path, ec);

	// Reset state for second path
	fs_unload();
 ASSERT_EQ(fs_init(true), 0);
	ASSERT_EQ(fs_getversion(), baseVersion_);

	// 2) Apply via KV-based (FDB-compatible) changelog using in-memory engine
	memkv::Engine engine;
	ASSERT_TRUE(seedKVChangelog(engine, {{v1, p1}, {v2, p2}})) << "Failed to seed KV changelog entries";

	master::load_changelogs_fdb(&engine);
	const uint64_t fdbVersion = fs_getversion();

	EXPECT_EQ(fileVersion, fdbVersion);
}

}  // namespace


#include "common/platform.h"  // NOLINT

#include <pthread.h>
#include <atomic>
#include <format>
#include <memory>
#include <mutex>
#include <string>

#include "fdb/fdb.h"
#include "fdb/fdb_codecs.h"
#include "fdb/fdb_context.h"
#include "fdb/fdb_schema.h"
#include "master/changelog_db.h"
#include "slogger/slogger.h"

namespace {

std::string encodeChangelogEntry(const uint64_t version) {
	return fdb::utils::composeKey({fdb::schema::CHANGELOG, fdb::schema::CHANGELOG_DATA_ENTRY},
	                              version);
}

std::string encodeChangelogIndex() {
	static auto changelogIndex =
	    fdb::utils::composeKey({fdb::schema::CHANGELOG}, fdb::schema::CHANGELOG_META_INDEX);
	return changelogIndex;
}

// // Helper conversions
kv::Key toKey(const std::string &strKey) { return {strKey.begin(), strKey.end()}; }

// as kv::Key and kv::Value are the same type, only one function is needed
std::string toString(const kv::Key &key) { return {key.begin(), key.end()}; }

kv::Value toValue(const std::string &strValue) { return {strValue.begin(), strValue.end()}; }

// Thread-safe singleton FDB context
std::shared_ptr<fdb::FDBContext> getSharedContext() {
	safs::log_info("Initializing FDB context");
	static const std::shared_ptr<fdb::FDBContext> context = fdb::FDBContext::create({});
	static int times = 1;
	safs::log_info("FDB init called {} times", times++);
	return context;
}

}  // namespace

struct ChangelogDb::ChangelogDbImpl {
	std::shared_ptr<fdb::FDBContext> context;
	std::shared_ptr<fdb::DB> db;
	std::string prefix{"changelog/"};
	std::string entriesPrefix;

	std::atomic<uint64_t> lastVersion{0};

	static constexpr size_t kMaxQueueSize = 10000;
	static constexpr size_t kBatchSize = 100;  // Write multiple entries per transaction

	ChangelogDbImpl() {
		safs::log_info("Initializing FDB changelog backend");
		try {
			entriesPrefix = fdb::utils::encodePrefix({
			    fdb::schema::CHANGELOG,
			    fdb::schema::CHANGELOG_DATA_ENTRY,
			});
			context = getSharedContext();
			if (!context) {
				safs::log_warn("FDB context unavailable - changelog writes disabled");
				return;
			}

			db = context->getDB();
			if (!db) {
				safs::log_warn("FDB DB unavailable - changelog writes disabled");
				return;
			}

			// Load the last written version
			loadLastVersion();

		} catch (const std::exception &e) {
			safs::log_err("FDB initialization failed: {}", e.what());
			db = nullptr;
		}
	}

	void put(uint64_t version, const std::string &entry) { writeOne(version, entry); }

private:
	void writeOne(uint64_t version, std::string entry) {
		safs::log_info("Writing FDB changelog version {}", version);
		writeBatch({{version, std::move(entry)}});
	}

	void loadLastVersion() {
		fdb::Transaction transaction(db.get());
		auto result = transaction.get(toKey(encodeChangelogIndex()),
		                              /*snapshot=*/true);

		if (result.has_value()) {
			lastVersion = std::stoull(toString(*result));
			safs::log_info("Loaded existing FDB changelog index: {}", lastVersion.load());
		} else {
			lastVersion = 0;
			safs::log_warn("FDB changelog is empty, starting from version 0");
		}
	}

	void writeBatch(const std::vector<std::pair<uint64_t, std::string>> &batch) {
		safs::log_info("FDB changelog writer thread writing batch of {} entries", batch.size());
		if (batch.empty()) { return; }

		fdb::Transaction transaction(db.get());

		uint64_t maxVersion = 0;

		// Set all entries in transaction
		for (const auto &[version, entry] : batch) {
			const std::string key = encodeChangelogEntry(version);
			transaction.set(toKey(key), toValue(entry));
			maxVersion = std::max(maxVersion, version);
		}

		// Update index
		transaction.set(toKey(encodeChangelogIndex()), toValue(std::to_string(maxVersion)));

		// Commit
		if (!transaction.commit()) {
			const int error = transaction.error();
			const std::string_view message = (error == -2) ? "timeout" : fdb::DB::errorMsg(error);
			safs::log_err("FDB commit failed for {} entries: {}", batch.size(), message);
			return;
		}

		// Update in-memory version only after successful commit
		lastVersion = maxVersion;
		safs::log_info("FDB changelog writer thread updated index to {}", lastVersion.load());
	}
};

ChangelogDb::ChangelogDb() : impl(std::make_unique<ChangelogDbImpl>()) {};

ChangelogDb::~ChangelogDb() = default;

void ChangelogDb::put(uint64_t version, const std::string &entry) const {
	safs::log_info("ChangelogDb::put() called: version={}, entry={}", version, entry);
	if (!impl || !impl->db) {
		safs::log_info("ChangelogDb::put(): FDB backend unavailable; skipping write");
		return;
	}
	try {
		impl->put(version, entry);
	} catch (const std::exception &e) {
		safs::log_exception(e, "Failed to enqueue changelog v={}", version);
	}
}

void ChangelogDb::flush() {
	safs::log_info("ChangelogDb::flush() called");
	// FDB transactions are always durable, so no need to flush
}

uint64_t ChangelogDb::getFirstLogVersion() const {
	safs::log_info("ChangelogDb::getFirstLogVersion() called");
	if (!impl || !impl->db) {
		safs::log_info("ChangelogDb::getFirstLogVersion(): FDB backend unavailable; returning 0");
		return 0;
	}

	try {
		fdb::Transaction transaction(impl->db.get());

		const auto [rangeBeginKey, rangeEndKey] = fdb::utils::makePrefixRange(impl->entriesPrefix);
		auto range = transaction.getRange(
		    kv::KeySelector(kv::toU8Vector(rangeBeginKey), /*inclusive=*/true, 0),
		    kv::KeySelector(kv::toU8Vector(rangeEndKey), /*inclusive=*/false, 0),
		    /*limit=*/1, 0, true, false, FDB_STREAMING_MODE_SMALL);

		const auto &keyValuePairs = range.getPairs();
		if (keyValuePairs.empty()) { return 0; }

		// Extract version from the key: "changelog/00000000000000001234"
		const auto &firstKeyVector = keyValuePairs.front().key;
		const size_t prefixLength = impl->entriesPrefix.size();

		// Expect layout: [prefix][BE64(version)]
		if (firstKeyVector.size() != prefixLength + fdb::utils::BE64_SIZE) { return 0; }

		// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
		const std::string_view versionBytes(
		    reinterpret_cast<const char *>(firstKeyVector.data()) + prefixLength,
		    fdb::utils::BE64_SIZE);
		// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
		return fdb::utils::decodeBE64(versionBytes);

	} catch (const std::exception &e) {
		safs::log_exception(e, "getFirstLogVersion failed");
		return 0;
	}
}

uint64_t ChangelogDb::getLastLogVersion() const {
	safs::log_info("ChangelogDb::getLastLogVersion() called");
	if (!impl || !impl->db) {
		safs::log_info("ChangelogDb::getLastLogVersion(): FDB backend unavailable; returning 0");
		return 0;
	}

	try {
		// Fast path: read from index
		fdb::Transaction transaction(impl->db.get());

		const std::string indexKey = encodeChangelogIndex();
		auto optionalValue = transaction.get(toKey(indexKey), /*snapshot=*/true);
		if (!!optionalValue) { return 0; }

		const auto &valueVector = *optionalValue;

		if (valueVector.empty()) { return 0; }

		// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
		const std::string_view versionByte(reinterpret_cast<const char *>(valueVector.data()),
		                                   fdb::utils::BE64_SIZE);
		return fdb::utils::decodeBE64(versionByte);
	} catch (const std::exception &e) {
		safs::log_exception(e, "getLastLogVersion failed");
		return 0;
	}
}


#include "common/platform.h"

#include <pthread.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "fdb/fdb.h"
#include "fdb/fdb_context.h"
#include "master/changelog_db.h"
#include "slogger/slogger.h"

namespace {

constexpr const char *INDEX_KEY = "__last_version__";

// Format version as a zero-padded key for lexicographic ordering
std::string makeKey(const std::string &prefix, uint64_t version) {
	return std::format("{}{:020}", prefix, version);
}

// Helper conversions
kv::Key toKey(const std::string &strKey) { return {strKey.begin(), strKey.end()}; }

// as kv::Key and kv::Value are the same type, only one function is needed
std::string toString(const kv::Key &key) { return {key.begin(), key.end()}; }

kv::Value toValue(const std::string &strValue) { return {strValue.begin(), strValue.end()}; }

// Thread-safe singleton FDB context
std::shared_ptr<fdb::FDBContext> getSharedContext() {
	safs::log_info("Initializing FDB context");
	static const std::shared_ptr<fdb::FDBContext> context = fdb::FDBContext::create({});
	return context;
}

}  // namespace

struct ChangelogDb::ChangelogDbImpl {
	std::shared_ptr<fdb::FDBContext> context;
	std::shared_ptr<fdb::DB> db;
	std::string prefix{"changelog/"};

	// Async writer
	std::thread worker;
	std::mutex mtx;
	std::condition_variable cv;
	std::deque<std::pair<uint64_t, std::string>> queue;
	std::atomic<bool> stop{false};
	std::atomic<uint64_t> lastVersion{0};

	static constexpr size_t kMaxQueueSize = 10000;
	static constexpr size_t kBatchSize = 100;  // Write multiple entries per transaction

	ChangelogDbImpl() {
		safs::log_info("Initializing FDB changelog backend");
		try {
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

			// Start background worker
			worker = std::thread([this]() {
				pthread_setname_np(pthread_self(), "changelog_fdb");
				this->run();
			});

		} catch (const std::exception &e) {
			safs::log_err("FDB initialization failed: {}", e.what());
			db = nullptr;
		}
	}

	~ChangelogDbImpl() {
		stop = true;
		cv.notify_all();
		if (worker.joinable()) { worker.join(); }
	}

	// Deleted copy constructor and assignment operator
	ChangelogDbImpl(const ChangelogDbImpl &other) = delete;
	ChangelogDbImpl &operator=(const ChangelogDbImpl &other) = delete;
	ChangelogDbImpl(ChangelogDbImpl &&other) = delete;
	ChangelogDbImpl &operator=(ChangelogDbImpl &&other) = delete;

	void enqueue(uint64_t version, std::string entry) {
		safs::log_info("Enqueueing FDB changelog version {}", version);
		const std::unique_lock<std::mutex> lock(mtx);
		if (queue.size() >= kMaxQueueSize) {
			safs::log_err("FDB changelog queue full ({}), dropping version {}", kMaxQueueSize,
			              version);
			return;
		}
		queue.emplace_back(version, std::move(entry));
		cv.notify_one();
	}

private:
	void loadLastVersion() {
		fdb::Transaction transaction(db.get());
		auto result = transaction.get(toKey(prefix + INDEX_KEY), /*snapshot=*/true);

		if (result.has_value()) {
			lastVersion = std::stoull(toString(*result));
			safs::log_info("Loaded existing FDB changelog index: {}", lastVersion.load());
		} else {
			lastVersion = 0;
			safs::log_warn("FDB changelog is empty, starting from version 0");
		}
	}

	void run() {
		std::vector<std::pair<uint64_t, std::string>> batch;
		batch.reserve(kBatchSize);

		while (!stop.load(std::memory_order_relaxed)) {
			// Collect batch
			{
				std::unique_lock<std::mutex> lock(mtx);
				cv.wait(lock,
				        [&] { return stop.load(std::memory_order_relaxed) || !queue.empty(); });

				if (stop && queue.empty()) { break; }

				// Drain up to kBatchSize items
				while (!queue.empty() && batch.size() < kBatchSize) {
					batch.push_back(std::move(queue.front()));
					queue.pop_front();
				}
			}

			if (batch.empty()) { continue; }

			// Write batch in single transaction
			writeBatch(batch);
			batch.clear();
		}
	}

	void writeBatch(const std::vector<std::pair<uint64_t, std::string>> &batch) {
		if (batch.empty()) { return; }

		fdb::Transaction transaction(db.get());

		uint64_t maxVersion = 0;

		// Set all entries in transaction
		for (const auto &[version, entry] : batch) {
			const std::string key = makeKey(prefix, version);
			transaction.set(toKey(key), toValue(entry));
			maxVersion = std::max(maxVersion, version);
		}

		// Update index
		transaction.set(toKey(prefix + INDEX_KEY), toValue(std::to_string(maxVersion)));

		// Commit
		if (!transaction.commit()) {
			const int error = transaction.error();
			const std::string_view message = (error == -2) ? "timeout" : fdb::DB::errorMsg(error);
			safs::log_err("FDB commit failed for {} entries: {}", batch.size(), message);
			return;
		}

		// Update in-memory version only after successful commit
		lastVersion = maxVersion;
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
		impl->enqueue(version, entry);
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

		// Get first key in range
		const std::string startKey = impl->prefix;
		const std::string endKey = impl->prefix + "\xff";

		auto range = transaction.getRange(kv::KeySelector(toKey(startKey), true, 0),
		                                  kv::KeySelector(toKey(endKey), false, 0),
		                                  /*limit=*/1, 0, true, false, FDB_STREAMING_MODE_SMALL);

		const auto &pairs = range.getPairs();

		if (pairs.empty()) { return 0; }

		// Extract version from the key: "changelog/00000000000000001234"
		const std::string keyStr = toString(pairs[0].key);

		// Skip index_key if present
		if (keyStr.find(INDEX_KEY) != std::string::npos) { return 0; }

		// Parse version from padded suffix
		if (keyStr.size() <= impl->prefix.size()) { return 0; }

		return std::stoull(keyStr.substr(impl->prefix.size()));

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
		auto result = transaction.get(toKey(impl->prefix + INDEX_KEY), /*snapshot=*/true);

		if (!result.has_value()) { return 0; }

		return std::stoull(toString(*result));

	} catch (const std::exception &e) {
		safs::log_exception(e, "getLastLogVersion failed");
		return 0;
	}
}

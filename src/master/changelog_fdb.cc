/*
   Copyright 2025      Leil Storage OÜ

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/platform.h"

#include "master/changelog_fdb.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "common/serialization.h"
#include "kv/itransaction.h"
#include "kv/kv_utils.h"
#include "master/changelog.h"
#include "master/filesystem.h"
#include "master/kv_common_keys.h"
#include "master/restore.h"
#include "master/exceptions.h"
#include "master/filesystem_metadata.h"
#include "master/filesystem_operations.h"
#include "slogger/slogger.h"

namespace master {

namespace {

// Decode big-endian uint64 from a key after a known prefix
static inline bool decodeVersionFromKey(const kv::Key &key, uint64_t &outVersion) {
	const size_t prefixLen = kChangelogPrefix.size();
	if (key.size() < prefixLen + sizeof(uint64_t)) { return false; }
	const uint8_t *ptr = key.data() + prefixLen;
	outVersion = get64bit(&ptr);
	return true;
}

}  // namespace

void load_changelogs_fdb(kv::IKVEngine *engine) {
	if (!engine) { return; }

	// Prepare range for all keys with the CHLG_ prefix: [ "CHLG_", "CHLG_\xff" ]
	std::string endStr = std::string(kChangelogPrefix) + "\xff";
	kv::KeySelector startSelector(kv::toBytes(kChangelogPrefix), true, 0);
	kv::KeySelector endSelector(kv::toBytes(endStr), true, 0);

	auto tr = engine->createReadWriteTransaction();
	if (!tr) { safs::log_err("load_changelogs_fdb: failed to create transaction"); return; }

	sassert(gMetadata->metadataVersion > 0);

	uint64_t first = 0;
	uint64_t id = 0;
	uint64_t skippedEntries = 0;
	uint64_t appliedEntries = 0;

	static constexpr int kPageSize = kv::kDefaultGetRangeLimit;
	while (true) {
		auto page = tr->getRange(startSelector, endSelector, kPageSize);
		const auto &pairs = page.getPairs();

		if (pairs.empty()) { break; }
		safs::log_info("FDB changelog: fetched {} entries in page", pairs.size());

		for (const auto &kvp : pairs) {
			uint64_t version = 0;
			if (!decodeVersionFromKey(kvp.key, version)) { continue; }
			id = version;
			if (version < fs_getversion()) {
				++skippedEntries;
				continue;
			} else if (!first) {
				first = version;
			}
			++appliedEntries;
			// Value contains the changelog payload string (without leading version). Ensure the leading
			// ": " is present for restore() to parse consistently with file-based loader.
			std::string raw(reinterpret_cast<const char *>(kvp.value.data()), kvp.value.size());
			std::string payload = ": " + raw;
			uint8_t status = restore("fdb", version, payload.c_str(), RestoreRigor::kIgnoreParseErrors);
			if (status != SAUNAFS_STATUS_OK) {
				throw MetadataConsistencyException("can't apply changelog from FDB", status);
			}
			// Persist the updated META_VERSION in big-endian to ensure consistent reads
			{
				auto wr = engine->createReadWriteTransaction();
				static kv::Key versionKey{kv::toBytes(kMetaVersionKey)};
				// Legacy semantics: META_VERSION stores the next expected version (last applied + 1)
				wr->set(versionKey, kv::toBytesLE<uint64_t>(version + 1));
				wr->commit();
				// DEBUG: verify encoding stored under META_VERSION
				{
					auto rd = engine->createReadWriteTransaction();
					auto chk = rd->get(versionKey);
					if (chk) {
						size_t n = chk->size();
						std::string hex;
						hex.reserve(n * 2);
						static const char *kHex = "0123456789ABCDEF";
						for (auto b : *chk) { hex.push_back(kHex[(b >> 4) & 0xF]); hex.push_back(kHex[b & 0xF]); }
						safs::log_info("FDB changelog: META_VERSION bytes ({}): {}", n, hex);
					}
				}
			}
		}

		if (!page.hasMore()) { break; }
		// Continue from last key (exclusive)
		const kv::Key &lastKey = pairs.back().key;
		startSelector = kv::KeySelector(lastKey, false, 0);
	}

	if (appliedEntries > 0) {
		safs::log_info("FDB changelog: %" PRIu64 " changes applied (%" PRIu64 " to %" PRIu64 ") , %" PRIu64 " skipped",
	               appliedEntries, first, id, skippedEntries);
	} else if (skippedEntries > 0) {
		safs::log_info("FDB changelog: skipped all %" PRIu64 " entries", skippedEntries);
	} else {
		safs::log_info("FDB changelog: no entries (ignored)");
	}
}

}  // namespace master

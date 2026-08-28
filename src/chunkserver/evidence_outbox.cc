/*
   Copyright 2026      Leil Storage OÜ

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

#include "chunkserver/evidence_outbox.h"
#include "common/platform.h"

#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

#include "common/datapack.h"
#include "common/test_event_stream.h"
#include "slogger/slogger.h"

namespace evidence_outbox {
namespace {

constexpr const char *kOutboxFilename = "evidence_outbox";
constexpr uint32_t kMagic = 0x5346454FU;  // "SFEO"
constexpr uint8_t kFormatVersion = 1;
/// One record on disk: checksum plus the fixed item payload.
constexpr size_t kPayloadSize =
    4 * sizeof(uint64_t) + sizeof(uint8_t) + sizeof(uint64_t) + sizeof(uint16_t) + sizeof(uint64_t);
constexpr size_t kRecordSize = sizeof(uint64_t) + kPayloadSize;
/// The visible admission bound. A correctness-bearing item is never discarded to make room;
/// beyond this, admission refuses and says so.
constexpr size_t kCapacity = 4096;

std::mutex gMutex;
std::deque<EvidenceItem> gItems;
uint64_t gNextSequence = 1;
bool gLoaded = false;

/// FNV-1a 64 over the record payload; a local integrity check, not a wire contract.
uint64_t payloadChecksum(const uint8_t *payload) {
	uint64_t hash = 0xcbf29ce484222325ULL;
	for (size_t i = 0; i < kPayloadSize; ++i) {
		hash ^= payload[i];
		hash *= 0x100000001b3ULL;
	}
	return hash;
}

void encodePayload(uint8_t *destination, const EvidenceItem &item) {
	put64bit(&destination, item.sequence);
	put64bit(&destination, item.incarnation);
	put64bit(&destination, item.originClaimSequence);
	put64bit(&destination, item.scanEpoch);
	put8bit(&destination, item.kind);
	put64bit(&destination, item.chunkId);
	put16bit(&destination, item.partType);
	put64bit(&destination, item.observedAtMs);
}

EvidenceItem decodePayload(const uint8_t *source) {
	EvidenceItem item;
	item.sequence = get64bit(&source);
	item.incarnation = get64bit(&source);
	item.originClaimSequence = get64bit(&source);
	item.scanEpoch = get64bit(&source);
	item.kind = get8bit(&source);
	item.chunkId = get64bit(&source);
	item.partType = get16bit(&source);
	item.observedAtMs = get64bit(&source);
	return item;
}

/// Rewrites the whole file with the write-fsync-rename idiom, so a crash at any moment leaves
/// either the old outbox or the new one, never a torn file. Caller holds the mutex.
bool persistLocked() {
	const std::string tmpPath = std::string(kOutboxFilename) + ".tmp";
	FILE *file = fopen(tmpPath.c_str(), "we");
	if (file == nullptr) {
		safs::log_err("evidence outbox: cannot write '{}'", tmpPath);
		return false;
	}
	uint8_t header[sizeof(uint32_t) + sizeof(uint8_t) + sizeof(uint64_t) + sizeof(uint16_t)];
	uint8_t *cursor = header;
	put32bit(&cursor, kMagic);
	put8bit(&cursor, kFormatVersion);
	put64bit(&cursor, gNextSequence);
	put16bit(&cursor, static_cast<uint16_t>(gItems.size()));
	bool ok = fwrite(header, sizeof(header), 1, file) == 1;
	for (const auto &item : gItems) {
		if (!ok) { break; }
		uint8_t record[kRecordSize];
		encodePayload(record + sizeof(uint64_t), item);
		uint8_t *sum = record;
		put64bit(&sum, payloadChecksum(record + sizeof(uint64_t)));
		ok = fwrite(record, kRecordSize, 1, file) == 1;
	}
	ok = fflush(file) == 0 && ok;
	ok = fsync(fileno(file)) == 0 && ok;
	ok = fclose(file) == 0 && ok;
	if (!ok || rename(tmpPath.c_str(), kOutboxFilename) != 0) {
		safs::log_err("evidence outbox: persist failed");
		unlink(tmpPath.c_str());
		return false;
	}
	return true;
}

}  // namespace

void init() {
	std::lock_guard<std::mutex> guard(gMutex);
	gItems.clear();
	gNextSequence = 1;
	gLoaded = true;
	FILE *file = fopen(kOutboxFilename, "re");
	if (file == nullptr) { return; }
	uint8_t header[sizeof(uint32_t) + sizeof(uint8_t) + sizeof(uint64_t) + sizeof(uint16_t)];
	if (fread(header, sizeof(header), 1, file) != 1) {
		fclose(file);
		safs::log_err("evidence outbox: unreadable header; starting empty");
		return;
	}
	const uint8_t *cursor = header;
	uint32_t magic = 0;
	get32bit(&cursor, magic);
	const uint8_t version = get8bit(&cursor);
	const uint64_t nextSequence = get64bit(&cursor);
	const uint16_t count = get16bit(&cursor);
	if (magic != kMagic || version != kFormatVersion) {
		fclose(file);
		safs::log_err("evidence outbox: wrong magic or version; starting empty");
		return;
	}
	gNextSequence = nextSequence;
	for (uint16_t i = 0; i < count; ++i) {
		uint8_t record[kRecordSize];
		if (fread(record, kRecordSize, 1, file) != 1) {
			safs::log_err("evidence outbox: truncated at record {}; keeping what was read", i);
			break;
		}
		const uint8_t *sum = record;
		if (get64bit(&sum) != payloadChecksum(record + sizeof(uint64_t))) {
			safs::log_err("evidence outbox: checksum mismatch at record {}; skipped", i);
			continue;
		}
		gItems.push_back(decodePayload(record + sizeof(uint64_t)));
	}
	fclose(file);
	safs::log_info("evidence outbox: loaded {} pending item(s), next sequence {}", gItems.size(),
	               gNextSequence);
}

bool append(EvidenceItem item) {
	std::lock_guard<std::mutex> guard(gMutex);
	if (!gLoaded) { return false; }
	if (gItems.size() >= kCapacity) {
		if (test_event_stream::enabled()) {
			test_event_stream::emit("evidence_outbox_full",
			                        {{"chunk", item.chunkId}, {"pending", gItems.size()}});
		}
		safs::log_warn("evidence outbox: full ({} items); observation refused visibly",
		               gItems.size());
		return false;
	}
	item.sequence = gNextSequence++;
	gItems.push_back(item);
	if (!persistLocked()) {
		gItems.pop_back();
		--gNextSequence;
		return false;
	}
	if (test_event_stream::enabled()) {
		test_event_stream::emit(
		    "evidence_outbox_appended",
		    {{"sequence", item.sequence}, {"chunk", item.chunkId}, {"kind", item.kind}});
	}
	return true;
}

std::vector<EvidenceItem> unacked(size_t limit) {
	std::lock_guard<std::mutex> guard(gMutex);
	const size_t count = std::min(limit, gItems.size());
	return std::vector<EvidenceItem>(gItems.begin(),
	                                 gItems.begin() + static_cast<std::ptrdiff_t>(count));
}

void ack(uint64_t upToSequence) {
	std::lock_guard<std::mutex> guard(gMutex);
	size_t dropped = 0;
	while (!gItems.empty() && gItems.front().sequence <= upToSequence) {
		gItems.pop_front();
		++dropped;
	}
	if (dropped == 0) { return; }
	persistLocked();
	if (test_event_stream::enabled()) {
		test_event_stream::emit(
		    "evidence_outbox_acked",
		    {{"up_to", upToSequence}, {"dropped", dropped}, {"pending", gItems.size()}});
	}
}

size_t pendingCount() {
	std::lock_guard<std::mutex> guard(gMutex);
	return gItems.size();
}

}  // namespace evidence_outbox

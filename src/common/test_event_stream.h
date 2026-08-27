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

#pragma once

#include "common/platform.h"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>

/// The H9 correlated-event stream: an opt-in test observation channel, never an
/// authority or synchronization dependency of production behavior. Each process
/// appends versioned JSON lines (one flushed record per event) so a harness can
/// capture a sequence cursor, install a fault, and wait for the exact boundary
/// event after that cursor instead of matching free-text logs.
namespace test_event_stream {

/// One typed correlation value; implicit constructions keep call sites terse.
struct Value {
	enum class Kind : uint8_t { kUnsigned, kSigned, kText };

	Value(uint64_t value) : kind(Kind::kUnsigned), unsignedValue(value) {}
	Value(uint32_t value) : kind(Kind::kUnsigned), unsignedValue(value) {}
	Value(uint16_t value) : kind(Kind::kUnsigned), unsignedValue(value) {}
	Value(uint8_t value) : kind(Kind::kUnsigned), unsignedValue(value) {}
	Value(int64_t value) : kind(Kind::kSigned), signedValue(value) {}
	Value(int value) : kind(Kind::kSigned), signedValue(value) {}
	Value(const char *value) : kind(Kind::kText), text(value) {}
	Value(std::string value) : kind(Kind::kText), text(std::move(value)) {}

	Kind kind;
	uint64_t unsignedValue = 0;
	int64_t signedValue = 0;
	std::string text;
};

using Field = std::pair<const char *, Value>;

/// Opens (or reopens) the stream. An empty path disables emission entirely, which is
/// the production default. Reloadable; safe to call from module init and reload.
void configure(const std::string &daemonKind, const std::string &path);

/// Stamps the stable daemon identity carried by every subsequent record. Zero values
/// mean not-yet-known and are omitted from the records.
void setIdentity(uint32_t stableId, uint64_t incarnation);

/// True when a stream is open; lets callers skip building fields for nothing.
bool enabled();

/// Appends one record with the common fields (schema version, daemon kind, identity,
/// strictly increasing per-process sequence, event name) plus the given correlations,
/// then flushes. Thread-safe; a write failure disables the stream and logs once.
void emit(const char *event, std::initializer_list<Field> fields);

}  // namespace test_event_stream

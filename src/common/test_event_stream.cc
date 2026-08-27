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

#include "common/platform.h"

#include "common/test_event_stream.h"

#include <cstdio>
#include <mutex>

#include "slogger/slogger.h"

namespace test_event_stream {
namespace {

constexpr uint32_t kSchemaVersion = 1;

std::mutex gMutex;
FILE *gFile = nullptr;
std::string gDaemonKind;
std::string gPath;
uint32_t gStableId = 0;
uint64_t gIncarnation = 0;
uint64_t gSequence = 0;
bool gWriteFailureLogged = false;

void appendEscaped(std::string &out, const std::string &text) {
	for (const char character : text) {
		switch (character) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (static_cast<unsigned char>(character) < 0x20) {
				char buffer[8];
				snprintf(buffer, sizeof(buffer), "\\u%04x", character);
				out += buffer;
			} else {
				out += character;
			}
		}
	}
}

void appendField(std::string &out, const char *name, const Value &value) {
	out += ",\"";
	appendEscaped(out, name);
	out += "\":";
	switch (value.kind) {
	case Value::Kind::kUnsigned:
		out += std::to_string(value.unsignedValue);
		break;
	case Value::Kind::kSigned:
		out += std::to_string(value.signedValue);
		break;
	case Value::Kind::kText:
		out += '"';
		appendEscaped(out, value.text);
		out += '"';
		break;
	}
}

}  // namespace

void configure(const std::string &daemonKind, const std::string &path) {
	const std::lock_guard<std::mutex> lock(gMutex);
	if (gFile != nullptr && path == gPath) {
		gDaemonKind = daemonKind;
		return;
	}
	if (gFile != nullptr) {
		fclose(gFile);
		gFile = nullptr;
	}
	gDaemonKind = daemonKind;
	gPath = path;
	gWriteFailureLogged = false;
	if (path.empty()) { return; }
	gFile = fopen(path.c_str(), "ae");
	if (gFile == nullptr) {
		safs::log_err("test event stream: cannot open '{}'", path);
		gPath.clear();
	}
}

void setIdentity(uint32_t stableId, uint64_t incarnation) {
	const std::lock_guard<std::mutex> lock(gMutex);
	gStableId = stableId;
	gIncarnation = incarnation;
}

bool enabled() {
	const std::lock_guard<std::mutex> lock(gMutex);
	return gFile != nullptr;
}

void emit(const char *event, std::initializer_list<Field> fields) {
	const std::lock_guard<std::mutex> lock(gMutex);
	if (gFile == nullptr) { return; }

	std::string line = "{\"v\":" + std::to_string(kSchemaVersion) + ",\"daemon\":\"";
	appendEscaped(line, gDaemonKind);
	line += '"';
	if (gStableId != 0) { line += ",\"stable_id\":" + std::to_string(gStableId); }
	if (gIncarnation != 0) { line += ",\"incarnation\":" + std::to_string(gIncarnation); }
	line += ",\"seq\":" + std::to_string(++gSequence);
	line += ",\"event\":\"";
	appendEscaped(line, event);
	line += '"';
	for (const Field &field : fields) { appendField(line, field.first, field.second); }
	line += "}\n";

	if (fputs(line.c_str(), gFile) < 0 || fflush(gFile) != 0) {
		if (!gWriteFailureLogged) {
			safs::log_err("test event stream: write to '{}' failed; disabling", gPath);
			gWriteFailureLogged = true;
		}
		fclose(gFile);
		gFile = nullptr;
	}
}

}  // namespace test_event_stream

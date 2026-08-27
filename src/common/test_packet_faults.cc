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

#include "common/test_packet_faults.h"

#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/test_event_stream.h"
#include "slogger/slogger.h"

namespace test_packet_faults {

namespace {

struct Rule {
	Verdict verdict = Verdict::kPass;
	uint32_t packetType = 0;
	bool anyType = false;
	uint64_t remaining = 0;  ///< 0 means the rule never retires
	uint32_t delayMilliseconds = 0;
};

std::mutex gMutex;
std::vector<Rule> gRules;
bool gArmed = false;

/// Splits on @p separator without dragging in a tokenizer; empty fields are kept so a
/// malformed field is rejected rather than silently skipped.
std::vector<std::string> split(const std::string &text, char separator) {
	std::vector<std::string> parts;
	std::string current;
	for (const char character : text) {
		if (character == separator) {
			parts.push_back(current);
			current.clear();
			continue;
		}
		current.push_back(character);
	}
	parts.push_back(current);
	return parts;
}

bool parseVerdict(const std::string &name, Verdict &verdict) {
	if (name == "drop") {
		verdict = Verdict::kDrop;
	} else if (name == "duplicate") {
		verdict = Verdict::kDuplicate;
	} else if (name == "delay") {
		verdict = Verdict::kDelay;
	} else {
		return false;
	}
	return true;
}

/// Parses one `action:type:count[:milliseconds]` field. A field that does not parse arms
/// nothing at all, so a typo cannot silently leave a run unfaulted or half faulted.
bool parseRule(const std::string &field, Rule &rule) {
	const auto parts = split(field, ':');
	if (parts.size() < 3 || parts.size() > 4) { return false; }
	if (!parseVerdict(parts[0], rule.verdict)) { return false; }

	rule.anyType = parts[1] == "*";
	try {
		if (!rule.anyType) { rule.packetType = std::stoul(parts[1]); }
		rule.remaining = std::stoull(parts[2]);
		rule.delayMilliseconds = parts.size() == 4 ? std::stoul(parts[3]) : 0;
	} catch (const std::exception &) { return false; }

	return rule.verdict != Verdict::kDelay || rule.delayMilliseconds > 0;
}

}  // namespace

void configure(bool testFaultsEnabled, const std::string &spec) {
	const std::lock_guard<std::mutex> lock(gMutex);
	gRules.clear();
	gArmed = false;

	if (spec.empty()) { return; }
	if (!testFaultsEnabled) {
		safs::log_err("packet fault spec refused: the test gate is not set");
		return;
	}

	std::vector<Rule> parsed;
	for (const auto &field : split(spec, ';')) {
		if (field.empty()) { continue; }
		Rule rule;
		if (!parseRule(field, rule)) {
			safs::log_err("packet fault spec refused: cannot parse '{}'", field);
			return;
		}
		parsed.push_back(rule);
	}

	gRules = std::move(parsed);
	gArmed = !gRules.empty();
	safs::log_warn("test seam active: {} packet fault rule(s) armed", gRules.size());
}

bool enabled() {
	const std::lock_guard<std::mutex> lock(gMutex);
	return gArmed;
}

Verdict decide(const char *stage, uint32_t packetType, uint32_t *delayMilliseconds) {
	Rule matched;
	{
		const std::lock_guard<std::mutex> lock(gMutex);
		if (!gArmed) { return Verdict::kPass; }

		auto rule = gRules.begin();
		for (; rule != gRules.end(); ++rule) {
			if (rule->anyType || rule->packetType == packetType) { break; }
		}
		if (rule == gRules.end()) { return Verdict::kPass; }

		matched = *rule;
		if (rule->remaining > 0 && --rule->remaining == 0) {
			gRules.erase(rule);
			gArmed = !gRules.empty();
		}
	}

	if (delayMilliseconds != nullptr) { *delayMilliseconds = matched.delayMilliseconds; }
	if (test_event_stream::enabled()) {
		const char *action = matched.verdict == Verdict::kDrop        ? "drop"
		                     : matched.verdict == Verdict::kDuplicate ? "duplicate"
		                                                             : "delay";
		test_event_stream::emit("packet_fault_applied",
		                        {{"stage", stage},
		                         {"action", action},
		                         {"packet_type", packetType},
		                         {"delay_ms", matched.delayMilliseconds}});
	}
	return matched.verdict;
}

}  // namespace test_packet_faults

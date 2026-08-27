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

#include "chunkserver/test_job_faults.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "common/test_event_stream.h"
#include "slogger/slogger.h"

namespace test_job_faults {
namespace {

struct Rule {
	std::atomic<uint32_t> remaining{0};
	std::atomic<uint32_t> milliseconds{0};
};

Rule gClientRule;
Rule gMasterRule;

void configureRule(Rule &rule, bool testFaultsEnabled, const std::string &spec,
                   const char *what) {
	rule.remaining.store(0, std::memory_order_relaxed);
	rule.milliseconds.store(0, std::memory_order_relaxed);
	if (spec.empty()) { return; }
	if (!testFaultsEnabled) {
		safs::log_warn("job hold spec ignored: the test gate is not set");
		return;
	}
	const auto separator = spec.find(':');
	if (separator == std::string::npos) {
		safs::log_warn("job hold spec {} is not count:milliseconds", spec);
		return;
	}
	const auto count = std::strtoul(spec.substr(0, separator).c_str(), nullptr, 10);
	const auto milliseconds = std::strtoul(spec.substr(separator + 1).c_str(), nullptr, 10);
	if (count == 0 || milliseconds == 0) {
		safs::log_warn("job hold spec {} arms nothing", spec);
		return;
	}
	rule.milliseconds.store(static_cast<uint32_t>(milliseconds), std::memory_order_relaxed);
	rule.remaining.store(static_cast<uint32_t>(count), std::memory_order_relaxed);
	safs::log_warn("test seam active: the next {} {} jobs are held {} ms each", count, what,
	               milliseconds);
}

}  // namespace

void configure(bool testFaultsEnabled, const std::string &spec) {
	configureRule(gClientRule, testFaultsEnabled, spec, "client");
}

void configureMaster(bool testFaultsEnabled, const std::string &spec) {
	configureRule(gMasterRule, testFaultsEnabled, spec, "master");
}

bool enabled() {
	return gClientRule.remaining.load(std::memory_order_relaxed) > 0 ||
	       gMasterRule.remaining.load(std::memory_order_relaxed) > 0;
}

void holdIfArmed(uint32_t jobId, bool clientFacing) {
	Rule &rule = clientFacing ? gClientRule : gMasterRule;
	uint32_t remaining = rule.remaining.load(std::memory_order_relaxed);
	while (remaining > 0 &&
	       !rule.remaining.compare_exchange_weak(remaining, remaining - 1,
	                                             std::memory_order_relaxed)) {}
	if (remaining == 0) { return; }
	const uint32_t milliseconds = rule.milliseconds.load(std::memory_order_relaxed);
	if (test_event_stream::enabled()) {
		test_event_stream::emit("job_hold_started", {{"job_id", jobId},
		                                             {"plane", clientFacing ? "client" : "master"},
		                                             {"milliseconds", milliseconds}});
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
	if (test_event_stream::enabled()) {
		test_event_stream::emit("job_hold_finished",
		                        {{"job_id", jobId},
		                         {"plane", clientFacing ? "client" : "master"}});
	}
}

}  // namespace test_job_faults

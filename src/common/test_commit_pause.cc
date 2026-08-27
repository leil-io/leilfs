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

#include "common/test_commit_pause.h"

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "common/test_event_stream.h"
#include "slogger/slogger.h"

namespace test_commit_pause {

namespace {

/// FoundationDB's commit_unknown_result. Named here so the seam speaks the same language the
/// production retry path already interprets, instead of inventing a private code.
constexpr int kCommitUnknownResult = 1021;

enum class Mode : uint8_t { kSuccess, kFailure, kUnknown };

Mode gMode = Mode::kSuccess;
uint64_t gRemaining = 0;
uint64_t gHoldMilliseconds = 0;
std::string gSite;  ///< Empty means every commit qualifies.
bool gArmed = false;

uint64_t nowMilliseconds() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

const char *modeName(Mode mode) {
	switch (mode) {
	case Mode::kSuccess: return "success";
	case Mode::kFailure: return "failure";
	case Mode::kUnknown: return "unknown";
	}
	return "unknown";
}

/// Wraps a real commit future and reports not ready until the hold expires. The wrapped future
/// is kept, never discarded: the commit really was submitted, and for the success and unknown
/// modes the durable outcome is whatever the backend actually did. Only what the caller is
/// told, and when, is under test.
class HeldCommitFuture : public kv::ICommitFuture {
public:
	HeldCommitFuture(std::unique_ptr<kv::ICommitFuture> inner, Mode mode, uint64_t releaseAt)
	    : inner_(std::move(inner)), mode_(mode), releaseAt_(releaseAt) {}

	bool isReady() override { return nowMilliseconds() >= releaseAt_; }

	bool getResult(int *error, bool *retryable) override {
		// Drain the real result first so the backend future is always finalized exactly once,
		// whatever this seam decides to report.
		int innerError = 0;
		bool innerRetryable = false;
		const bool innerOk = inner_->getResult(&innerError, &innerRetryable);

		if (test_event_stream::enabled()) {
			test_event_stream::emit("commit_pause_released",
			                        {{"mode", modeName(mode_)},
			                         {"backend_committed", static_cast<uint8_t>(innerOk ? 1 : 0)},
			                         {"backend_error", innerError}});
		}

		switch (mode_) {
		case Mode::kSuccess:
			if (error != nullptr) { *error = innerError; }
			if (retryable != nullptr) { *retryable = innerRetryable; }
			return innerOk;
		case Mode::kFailure:
			if (error != nullptr) { *error = innerError != 0 ? innerError : -1; }
			if (retryable != nullptr) { *retryable = false; }
			return false;
		case Mode::kUnknown:
			// The commit may or may not have landed, which is exactly what the caller must
			// resolve by rereading rather than assuming.
			if (error != nullptr) { *error = kCommitUnknownResult; }
			if (retryable != nullptr) { *retryable = true; }
			return false;
		}
		return false;
	}

	void setReadyCallback(void (*callback)(void *), void *arg) override {
		// The wakeup would fire on the backend's schedule, not this seam's, and waking early
		// only means one more poll that still reports not ready. The event loop's poll timeout
		// is what releases a held commit.
		(void)callback;
		(void)arg;
	}

private:
	std::unique_ptr<kv::ICommitFuture> inner_;
	Mode mode_;
	uint64_t releaseAt_;
};

}  // namespace

void holdAfterDurableCommit(const char *site) {
	if (!gArmed) { return; }
	if (!gSite.empty() && (site == nullptr || gSite != site)) { return; }
	const Mode mode = gMode;
	if (--gRemaining == 0) { gArmed = false; }
	if (test_event_stream::enabled()) {
		test_event_stream::emit("commit_pause_armed",
		                        {{"mode", modeName(mode)},
		                         {"hold_ms", gHoldMilliseconds},
		                         {"site", site == nullptr ? "unnamed" : site},
		                         {"synchronous", static_cast<uint8_t>(1)}});
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(gHoldMilliseconds));
	if (test_event_stream::enabled()) {
		test_event_stream::emit("commit_pause_released",
		                        {{"mode", modeName(mode)},
		                         {"backend_committed", static_cast<uint8_t>(1)},
		                         {"backend_error", 0}});
	}
}

void configure(bool testPausesEnabled, const std::string &spec) {
	gArmed = false;
	gRemaining = 0;

	if (spec.empty()) { return; }
	if (!testPausesEnabled) {
		safs::log_err("commit pause spec refused: the test gate is not set");
		return;
	}

	std::vector<std::string> parts;
	std::string current;
	for (const char character : spec) {
		if (character == ':') {
			parts.push_back(current);
			current.clear();
			continue;
		}
		current.push_back(character);
	}
	parts.push_back(current);

	if (parts.size() != 3 && parts.size() != 4) {
		safs::log_err("commit pause spec refused: expected mode:count:milliseconds[:site]");
		return;
	}
	gSite = parts.size() == 4 ? parts[3] : std::string();
	if (!gSite.empty() && gSite != "batch" && gSite != "effects" && gSite != "retirement") {
		safs::log_err("commit pause spec refused: unknown site '{}'", gSite);
		return;
	}
	if (parts[0] == "success") {
		gMode = Mode::kSuccess;
	} else if (parts[0] == "failure" || parts[0] == "unknown") {
		// Measured, not assumed: reporting an invented outcome here lets the caller enter a
		// recovery path for a transaction that is not in the matching state. Injecting a
		// commit unknown above a commit that really landed made the batch path replay a
		// committed transaction and wedged the metadata server for minutes. An invented
		// outcome has to come from the backend boundary, where the transaction state and the
		// error agree; this seam owns the window, not the verdict.
		safs::log_err("commit pause spec refused: mode '{}' must be injected at the backend "
		              "boundary, not at the caller",
		              parts[0]);
		return;
	} else {
		safs::log_err("commit pause spec refused: unknown mode '{}'", parts[0]);
		return;
	}
	try {
		gRemaining = std::stoull(parts[1]);
		gHoldMilliseconds = std::stoull(parts[2]);
	} catch (const std::exception &) {
		safs::log_err("commit pause spec refused: cannot parse count or milliseconds");
		return;
	}

	gArmed = gRemaining > 0;
	if (gArmed) {
		safs::log_warn("test seam active: holding {} commit(s) at site '{}' for {} ms, releasing as {}",
		               gRemaining, gSite.empty() ? "any" : gSite, gHoldMilliseconds,
		               modeName(gMode));
	}
}

bool enabled() { return gArmed; }

std::unique_ptr<kv::ICommitFuture> hold(std::unique_ptr<kv::ICommitFuture> future,
                                        const char *site) {
	if (!gArmed || future == nullptr) { return future; }
	// A rule that names a site lets every other commit pass untouched and unconsumed.
	if (!gSite.empty() && (site == nullptr || gSite != site)) { return future; }

	const Mode mode = gMode;
	if (--gRemaining == 0) { gArmed = false; }

	if (test_event_stream::enabled()) {
		test_event_stream::emit("commit_pause_armed",
		                        {{"mode", modeName(mode)},
		                         {"hold_ms", gHoldMilliseconds},
		                         {"site", site == nullptr ? "unnamed" : site}});
	}
	return std::make_unique<HeldCommitFuture>(std::move(future), mode,
	                                          nowMilliseconds() + gHoldMilliseconds);
}

}  // namespace test_commit_pause

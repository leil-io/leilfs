/*
   Copyright 2026      Leil Storage OÜ

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/platform.h"

#include <algorithm>
#include <chrono>
#include <optional>

#include "common/chunkserver_id.h"
#include "common/saunafs_version.h"

/// Registration sequence of one chunkserver connection: host accepted, identity pending when the
/// backend asked for it and the peer can answer, then completed exactly once at the first space
/// report. Kept apart from the connection so the sequence can be unit tested and cannot run twice.
class ChunkserverRegistrationState {
public:
	using Clock = std::chrono::steady_clock;
	using TimePoint = Clock::time_point;

	/// Accepts the host registration. False on a second host registration, which the caller
	/// treats as a protocol violation.
	bool begin(bool requestIdentity, uint32_t peerVersion, TimePoint now = Clock::now()) {
		if (hostAccepted_) { return false; }

		hostAccepted_ = true;
		identityPending_ = requestIdentity && peerVersion >= kFirstVersionWithChunkserverIdentity;
		if (identityPending_) { identityStarted_ = now; }
		return true;
	}

	/// True while the identity reply is awaited; only a keepalive is accepted meanwhile.
	bool identityPending() const { return identityPending_; }

	/// Stores the identity reply. False when none was requested or the identity is all zero, which
	/// a well-formed chunkserver never sends since it generates and persists one at startup.
	bool acceptIdentity(const chunkserver::ChunkserverId &identity) {
		const bool isZero =
		    std::all_of(identity.begin(), identity.end(), [](uint8_t byte) { return byte == 0; });
		if (!identityPending_ || isZero) { return false; }

		identity_ = identity;
		identityPending_ = false;
		return true;
	}

	const std::optional<chunkserver::ChunkserverId> &identity() const { return identity_; }

	/// True when the identity reply is overdue by the peer's own keepalive timeout.
	bool identityExpired(uint32_t timeoutMs, TimePoint now = Clock::now()) const {
		return identityPending_ && identityStarted_.has_value() &&
		       now - *identityStarted_ >= std::chrono::milliseconds(timeoutMs);
	}

	/// True exactly once, when the host is accepted and no identity is pending; callers run the
	/// registration completion seam on that transition only.
	bool markRegistrationComplete() {
		if (!hostAccepted_ || identityPending_ || registrationCompleted_) { return false; }

		registrationCompleted_ = true;
		return true;
	}

private:
	bool hostAccepted_{false};
	bool identityPending_{false};
	bool registrationCompleted_{false};
	std::optional<chunkserver::ChunkserverId> identity_;
	/// When the identity request went out; drives identityExpired.
	std::optional<TimePoint> identityStarted_;
};

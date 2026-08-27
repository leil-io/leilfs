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

#include <memory>
#include <string>

#include "kv/ifuture.h"

/// The H8 asynchronous commit seam: holds one commit result between the moment the commit was
/// submitted and the moment its caller learns the outcome, then releases it as a known
/// success, a known failure, or a commit whose outcome is unknown.
///
/// That window is where the hard cases live. A metadata server that submitted a commit and has
/// not yet been told what happened must not adopt, publish or dispatch anything that depends
/// on it, and a commit reported as unknown must be resolved by rereading rather than assumed
/// either way. Neither behavior can be tested without holding the window open on purpose.
///
/// Off by default and fail closed: a spec is honored only when the explicit test gate is set.
namespace test_commit_pause {

/// Applies the configuration; reloadable. The spec is `mode:count:milliseconds[:site]`, where
/// count is how many commits the rule holds before it retires, milliseconds is how long each is
/// held, and the optional site names which commits qualify: `batch` for the asynchronous window
/// of any client batch (before its outcome is known), `effects` for the window after any durable
/// commit and before its effects run, `retirement` for that same window on a commit that retires a
/// chunk's published set (H11: between the commit that owes deletions and the commands that pay
/// them). Without a site every window qualifies. A nonempty spec is honored only when @p testPausesEnabled; otherwise it is
/// refused and nothing is armed.
///
/// The only mode this seam can honestly serve is `success`: it holds the window open and then
/// tells the truth. `failure` and `unknown` are refused, because inventing an outcome here
/// sends the caller into a recovery path for a transaction that is not in the state that
/// outcome implies. Those belong at the backend boundary, where the error and the transaction
/// state agree.
void configure(bool testPausesEnabled, const std::string &spec);

/// True when a rule is still armed; lets callers skip wrapping entirely.
bool enabled();

/// Returns @p future untouched when nothing is armed or the armed rule names another site,
/// otherwise a future that reports not ready until the hold expires and then yields the
/// configured outcome. Emits the H9 commit_pause_armed and commit_pause_released events with the
/// site, so a harness waits for the window to open and close instead of guessing at it.
std::unique_ptr<kv::ICommitFuture> hold(std::unique_ptr<kv::ICommitFuture> future,
                                        const char *site);

/// The same window for a commit that was awaited synchronously: called right after the backend
/// confirmed the commit and before its effects run, it blocks the caller for the configured hold
/// when a rule is armed for @p site, emitting the same two events. Blocking is the point: the
/// process is exactly where a crash between commit and effects would leave it.
void holdAfterDurableCommit(const char *site);

}  // namespace test_commit_pause

/*
   Copyright 2026 Leil Storage OÜ

   This file is part of LeilFS.

   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/platform.h"

namespace mount_reconnect {

/// Tries known servers before resolving and trying the configured one, even if already tried:
/// the lookup may refresh the source address. A true attempt result is terminal, including
/// a definitive loss of the existing session.
template <typename Candidates, typename TryServer, typename ResolveConfigured>
void tryKnownThenConfigured(const Candidates &known, TryServer tryServer,
                            ResolveConfigured resolveConfigured) {
	for (const auto &server : known) {
		if (tryServer(server)) { return; }
	}

	// A slow or failing name service must never delay a server the client already knows.
	const auto configured = resolveConfigured();
	if (configured) { tryServer(*configured); }
}

}  // namespace mount_reconnect

/*

   Copyright 2024 Leil Storage OÜ

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
#include "metrics/metrics.h"

#ifdef HAVE_PROMETHEUS
#include <prometheus/counter.h>
#include <prometheus/registry.h>

namespace metrics {

struct Master {
	Master() = default;
	Master(std::shared_ptr<prometheus::Registry>& registry);

	// Metric(s)
	CounterFamily *packetClientCounter{nullptr};
	CounterFamily *byteClientCounter{nullptr};
	CounterFamily *filesystemCounter{nullptr};
	CounterFamily *chunkCounter{nullptr};

	// Master Counters
	std::array<Counter, static_cast<unsigned int>(Counter::Master::KEY_END) + 1> masterCounters;
};
}
#endif

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

struct Master : ServiceType {
	Master(const Master &) = default;
	Master(Master &&) = delete;
	Master &operator=(const Master &) = default;
	Master &operator=(Master &&) = delete;
	Master(std::shared_ptr<prometheus::Registry>& registry);

	Counter& get(chunkserver::Counters  /*key*/) override {
		throw std::logic_error("Master should not call with chunkserver counters");
	}
	Counter& get(master::Counters key) override {
		return counters[static_cast<uint8_t>(key)];
	};
	Gauge& get(chunkserver::Gauges  /*key*/) override {
		throw std::logic_error("Not implemented");
	};
	Gauge& get(master::Gauges  /*key*/) override {
		throw std::logic_error("Not implemented");
	};

	// Metric(s)
	CounterFamily *packet_client_counter{nullptr};
	CounterFamily *byte_client_counter{nullptr};
	CounterFamily *filesystem_counter{nullptr};
	CounterFamily *chunkCounter{nullptr};

	// Master Counters
	std::array<Counter, static_cast<uint8_t>(master::Counters::KEY_END) + 1> counters;
	~Master() override = default;
};
}
#endif

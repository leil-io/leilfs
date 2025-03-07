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

	Counter& getCounter(const uint8_t key) override {
		return counters.at(key);
	};
	Gauge& getGauge(const uint8_t key) override {
		return gauges.at(key);
	}

	// Metric(s)
	CounterFamily *packets_client_total{nullptr};
	CounterFamily *bytes_client_total{nullptr};
	CounterFamily *filesystem_stats_total{nullptr};
	CounterFamily *chunk_operations_total{nullptr};

	// Master client metrics
	std::array<Counter, static_cast<uint8_t>(metadata::Counters::KEY_END) + 1> counters;
	std::array<Gauge, static_cast<uint8_t>(metadata::Gauges::KEY_END) + 1> gauges;
	~Master() override = default;
};
}
#endif

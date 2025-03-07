#pragma once
#ifdef HAVE_PROMETHEUS
#include <prometheus/counter.h>
#include <prometheus/registry.h>
#include <cstdint>
#include <stdexcept>
#include "metrics/metrics.h"

namespace metrics {

struct Chunkserver : ServiceType {
public:
	Chunkserver(const Chunkserver &) = default;
	Chunkserver(Chunkserver &&) = delete;
	Chunkserver &operator=(const Chunkserver &) = default;
	Chunkserver &operator=(Chunkserver &&) = delete;
	Chunkserver(std::shared_ptr<prometheus::Registry> &registry);

	Counter& getCounter(uint8_t key) override {
		return counters.at(key);
	}
	Gauge& getGauge(uint8_t key) override {
		return gauges.at(key);
	};

	~Chunkserver() override = default;
private:
	// Metric(s)
	// WARNING: Changing the variable names will change what appears in
	// prometheus
	prometheus::Family<prometheus::Counter> *network_bytes_total{nullptr};
	prometheus::Family<prometheus::Counter> *network_operations_total{nullptr};
	prometheus::Family<prometheus::Counter> *operations_jobs{nullptr};
	prometheus::Family<prometheus::Counter> *chunk_operations_total{nullptr};
	prometheus::Family<prometheus::Counter> *hdd_operations_total{nullptr};
	prometheus::Family<prometheus::Counter> *hdd_bytes_total{nullptr};
	prometheus::Family<prometheus::Counter> *hdd_time_total{nullptr};
	prometheus::Family<prometheus::Counter> *replications_total{nullptr};

	// Counters
	std::array<Counter, static_cast<uint8_t>(chunkserver::Counters::KEY_END) + 1> counters;
	std::array<Gauge, static_cast<uint8_t>(chunkserver::Counters::KEY_END) + 1> gauges;
  };
}
#endif

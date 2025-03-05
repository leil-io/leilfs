#pragma once
#ifdef HAVE_PROMETHEUS
#include <stdexcept>
#include "metrics/metrics.h"
#include <prometheus/counter.h>
#include <prometheus/registry.h>

namespace metrics {

struct Chunkserver : ServiceType {
public:
	Chunkserver(const Chunkserver &) = default;
	Chunkserver(Chunkserver &&) = delete;
	Chunkserver &operator=(const Chunkserver &) = default;
	Chunkserver &operator=(Chunkserver &&) = delete;
	Chunkserver(std::shared_ptr<prometheus::Registry> &registry);

	Counter& get(chunkserver::Counters key) override {
		return counters[static_cast<uint8_t>(key)];
	}
	Counter& get(master::Counters /*key*/) override {
		throw std::logic_error("Chunkserver should not call with master counters");
	};
	Gauge& get(chunkserver::Gauges  /*key*/) override {
		throw std::logic_error("Not implemented");
	};
	Gauge& get(master::Gauges  /*key*/) override {
		throw std::logic_error("Not implemented");
	};

	~Chunkserver() override = default;
private:
	// Metric(s)
	// WARNING: Changing the variable names will change what appears in
	// prometheus
	prometheus::Family<prometheus::Counter> *network_bytes_total{nullptr};
	prometheus::Family<prometheus::Counter> *network_operations_total{nullptr};
	prometheus::Family<prometheus::Counter> *local_bytes_total{nullptr};
	prometheus::Family<prometheus::Counter> *max_operations_jobs_total{nullptr};
	prometheus::Family<prometheus::Counter> *chunk_operations_total{nullptr};
	prometheus::Family<prometheus::Counter> *hdd_operations_total{nullptr};
	prometheus::Family<prometheus::Counter> *hdd_bytes_total{nullptr};
	prometheus::Family<prometheus::Counter> *replications_total{nullptr};

	// Counters
	std::array<Counter, static_cast<uint8_t>(chunkserver::Counters::KEY_END) + 1> counters;
  };
}
#endif

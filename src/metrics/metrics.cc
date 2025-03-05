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

// This library provides a means of manipulating Prometheus metrics safely and
// efficiently.
// The two key restraints in the design are:
// 1. Prefer compile time costs/safety over initialization time costs/safety,
// and avoid runtime costs except in accessing the metrics themselves (but
// minimize them).
// 2. Do not allow dynamic dimensional data on runtime (i.e, calling Add after
// initialization)
//
// This should allow placing metric gathering functions pretty much everywhere,
// without worry for performance impact. To do this, it uses a lookup table
// using enums instead of a map. See master.cc and master.h for an example
// implementation for adding new metrics.

#ifdef HAVE_PROMETHEUS
#include "chunkserver.h"
#include "master.h"
#include <prometheus/counter.h>
#include <prometheus/detail/builder.h>
#include <prometheus/family.h>
#include <prometheus/registry.h>
#include <pthread.h>
#include <unistd.h>
#include <exception>
#include <memory>
#endif

#include "metrics.h"
#include "slogger/slogger.h"


constexpr auto THREAD_SLEEP_TIME_MS = 100;

namespace metrics {

static std::unique_ptr<std::jthread>
    gMetricsMainThread;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

void destroy() {
	if (gMetricsMainThread != nullptr) {
		gMetricsMainThread->request_stop();
	}
}

#ifndef HAVE_PROMETHEUS
void init(const char* /* unused */, const Type /* unused */) {
	safs::log_err(
	    "could not setup prometheus server: Prometheus isn't compiled with "
	    "this program");
}
}
#else

class PrometheusMetrics {
public:

	PrometheusMetrics()
	    :
	      registry(std::make_shared<prometheus::Registry>()) {
	}

	void initialize_service(Type type) {
		switch (type) {
			case MASTER:
				service = std::make_unique<Master>(registry);
				break;
			case CHUNKSERVER:
				service = std::make_unique<Chunkserver>(registry);
				break;
		}
	}

	std::shared_ptr<prometheus::Registry> getRegistry() {
		return registry;
	}

	// Master metrics
	std::unique_ptr<ServiceType> service; // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes)

private:
	// Registry
	std::shared_ptr<prometheus::Registry> registry;
};
PrometheusMetrics gPrometheusMetrics;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

template <typename T>
void Counter::increment(T key, double n) {
	// Safe as all values are constructed at specific keys, however a check
	// needs to be made whether the actual counter initialized or not (for
	// whatever reason)
	auto counter = gPrometheusMetrics.service->get(key); // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
	if (counter.counter_ != nullptr) { counter.counter_->Increment(n); }
}

template <typename T>
void Gauge::set(T key, double n) {
	// Safe as all values are constructed at specific keys, however a check
	// needs to be made whether the actual counter initialized or not (for
	// whatever reason)
	auto counter = gPrometheusMetrics.service->get(key); // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
	if (counter.gauge_ != nullptr) { counter.gauge_->Set(n); }
}

void Gauge::set(chunkserver::Gauges key, double n) {
	auto counter = gPrometheusMetrics.service->get(key); // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
	if (counter.gauge_ != nullptr) { counter.gauge_->Set(n); }
}

void prometheus_loop(const std::stop_token& stop, const char* host) {
	try {
		// create an http server
		prometheus::Exposer exposer{host};

		exposer.RegisterCollectable(gPrometheusMetrics.getRegistry());
		safs::log_info("started prometheus server");

		while (!stop.stop_requested()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(THREAD_SLEEP_TIME_MS));
		};
	} catch (std::exception &e) {
		safs::log_err("could not setup prometheus server: {}", e.what());
	}
}

void init(const char* host, Type type) {
	gMetricsMainThread = std::make_unique<std::jthread>(std::jthread(prometheus_loop, host));
	gPrometheusMetrics.initialize_service(type);
}

}
#endif
template void metrics::Counter::increment<metrics::master::Counters>
	(master::Counters key, double n);
template void metrics::Counter::increment<metrics::chunkserver::Counters>
	(chunkserver::Counters key, double n);
template void metrics::Gauge::set<metrics::master::Gauges>
	(master::Gauges key, double n);
template void metrics::Gauge::set<metrics::chunkserver::Gauges>
	(chunkserver::Gauges key, double n);

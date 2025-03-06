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
#include <prometheus/gauge.h>
#ifdef HAVE_PROMETHEUS
#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/registry.h>
#include <prometheus/family.h>

using CounterFamily = prometheus::Family<prometheus::Counter>;

#endif

namespace metrics {

enum Type : uint8_t {
	MASTER,
	CHUNKSERVER,
};

namespace metadata {

enum class Counters : uint8_t {
	KEY_START = 0,      // Used internally, has no effect
	CHUNK_DELETE,       // Chunk deletion operations
	CHUNK_REPLICATE,    // Chunk replication operations
	FS_STATFS,          // Filesystem STATFS operations
	FS_GETATTR,         // Filesystem GETATTR operations
	FS_SETATTR,         // Filesystem SETATTR operations
	FS_LOOKUP,          // Filesystem LOOKUP operations
	FS_MKDIR,           // Filesystem MKDIR operations
	FS_RMDIR,           // Filesystem RMDIR operations
	FS_SYMLINK,         // Filesystem SYMLINK operations
	FS_READLINK,        // Filesystem READLINK operations
	FS_MKNOD,           // Filesystem MKNOD operations
	FS_UNLINK,          // Filesystem UNLINK operations
	FS_RENAME,          // Filesystem RENAME operations
	FS_LINK,            // Filesystem LINK operations
	FS_READDIR,         // Filesystem READDIR operations
	FS_OPEN,            // Filesystem OPEN operations
	FS_READ,            // Filesystem READ operations
	FS_WRITE,           // Filesystem WRITE operations
	CLIENT_RX_PACKETS,  // Packets (i.e messages) received from
	                    // client
	CLIENT_TX_PACKETS,  // Packets (i.e messages) sent to client
	CLIENT_RX_BYTES,    // Bytes received from client
	CLIENT_TX_BYTES,    // Bytes sent to client
	KEY_END,            // Used internally, has no effect
};

enum class Gauges : uint8_t {
	GAUGE_KEY_START = 0,
	GAUGE_KEY_END,
};

}

namespace chunkserver {

enum class Counters : uint8_t {
	KEY_START = 0,                     // Used internally, has no effect
	MASTER_RX_BYTES,                   // Received bytes from master
	MASTER_TX_BYTES,                   // Sent bytes to master
	ANY_RX_BYTES,                      // Bytes from client(s)/chunkserver(s)
	ANY_TX_BYTES,                      // Bytes to client(s)/chunkserver(s)
	CHUNKSERVER_HIGH_LEVEL_READ_OPS,   // ???
	CHUNKSERVER_HIGH_LEVEL_WRITE_OPS,  // ???
	CHUNKSERVER_LOW_LEVEL_READ_OPS,    // ???
	CHUNKSERVER_LOW_LEVEL_WRITE_OPS,   // ???
	OVERHEAD_BYTES_READ,               // ???
	OVERHEAD_BYTES_WRITE,              // ???
	OVERHEAD_OPERATIONS_READ,          // ???
	OVERHEAD_OPERATIONS_WRITE,         // ???
	HDD_TOTAL_BYTES_READ,
	HDD_TOTAL_BYTES_WRITE,
	HDD_TOTAL_LOW_LEVEL_READS,
	HDD_TOTAL_LOW_LEVEL_WRITES,
	REPLICATIONS,
	OPERATION_CREATE,                  // Chunk create operations
	OPERATION_DELETE,                  // Chunk delete operations
	OPERATION_VERSION,                 // Chunk version operation
	OPERATION_DUPLICATE,               // Chunk duplicate operation
	OPERATION_TRUNCATE,                // Chunk truncate operations
	OPERATION_DUPLICATE_TRUNCATE,      // Chunk duplicate truncate operations
	OPERATION_TEST,                    // Chunk test operations
	KEY_END,                           // Used internally, has no effect
};

enum Gauges : uint8_t {
	GAUGE_KEY_START = 0,
	CHUNKSERVER_MAX_SERVER_JOBS,
	MASTER_MAX_OPERATION_JOBS,
	GAUGE_KEY_END,
};

}

class Counter {
public:
#ifdef HAVE_PROMETHEUS
	Counter() : counter_(nullptr) {};
	Counter(const prometheus::Labels &labels, CounterFamily *family) :
		counter_(&family->Add(labels)) {};

	template <typename T>
	static void increment(T key, double n = 1);

private:
	prometheus::Counter* counter_;
#else
	// Dummy methods for packages without prometheus
	explicit Counter() = default;

	template <typename T>
	static void increment(T /*unused*/, double  /*unused*/= 1) {
	}
#endif
};

class Gauge {
public:
#ifdef HAVE_PROMETHEUS
	Gauge() : gauge_(nullptr) {};
	Gauge(const prometheus::Labels &labels,
	        prometheus::Family<prometheus::Gauge> *family)
	    : gauge_(&family->Add(labels)) {};

	template <typename T>
	static void set(T key, double n = 1);
	static void set(chunkserver::Gauges key, double n = 1);

private:
	prometheus::Gauge* gauge_;
#else
	// Dummy methods for packages without prometheus
	explicit Gauge() = default;

	static void increment(master::Counters /*unused*/, double  /*unused*/= 1) {
	}
#endif
};

// Interface for various service types (chunkserver, master etc.)
struct ServiceType {
	ServiceType() = default;
	ServiceType(const ServiceType &) = default;
	ServiceType(ServiceType &&) = delete;
	ServiceType &operator=(const ServiceType &) = default;
	ServiceType &operator=(ServiceType &&) = delete;
	virtual Counter& getCounter(uint8_t key) = 0;
	virtual Gauge& getGauge(uint8_t key) = 0;
	virtual ~ServiceType() = default;
};

void init(const char* host, Type type);
void destroy();

} // metrics

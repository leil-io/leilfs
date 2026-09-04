/*
   Copyright 2026 Urmas Rist <urmas@urist.ee>

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once
#include <atomic>
#include <cstdint>
#include <string_view>

namespace metrics {

enum class MetricType : uint8_t {
	UINT64,
	// This is currently not used, but is shown demonstratively how two or more
	// types work in this system
	FLOAT64,
};

struct Metric {
	const std::string_view name;
	const MetricType type;
	// I couldn't figure out how to get std::variant working with atomics. If
	// someone can figure it out, feel free to change this to std::variant
	union {
		std::atomic<uint64_t> u64;
		std::atomic<double> f64;
	} value;
};

struct MetricSerialized {
	uint32_t size;
	unsigned char *data;
};

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define METRIC_METADATA_UINT64_LIST(X) \
    X(METADATA_CLIENT_BYTES_RECEIVED_INCREMENT) /* Number of bytes received from client to metadata server */         \
    X(METADATA_CLIENT_BYTES_SENT_INCREMENT)     /* Number of bytes send to client from metadata server */             \
    X(METADATA_CLIENT_PACKETS_RECEIVED_INCREMENT) /* Number of bytes received from client to metadata server */         \
    X(METADATA_CLIENT_PACKETS_SENT_INCREMENT)     /* Number of bytes send to client from metadata server */             \
    X(NUMBER_OF_CHUNKS_GAUGE)                   /* Number of chunks (master/chunkserver) */                           \
	X(METADATA_CHUNK_DELETE_INCREMENT)          /* Chunk deletion operations */                                       \
	X(METADATA_CHUNK_REPLICATE_INCREMENT)       /* Chunk replication operations */                                    \
	X(METADATA_FS_STATFS_INCREMENT)             /* Metadata server STATFS operations */                               \
	X(METADATA_FS_GETATTR_INCREMENT)            /* Metadata server GETATTR operations */                              \
	X(METADATA_FS_SETATTR_INCREMENT)            /* Metadata server SETATTR operations */                              \
	X(METADATA_FS_LOOKUP_INCREMENT)             /* Metadata server LOOKUP operations */                               \
	X(METADATA_FS_MKDIR_INCREMENT)              /* Metadata server MKDIR operations */                                \
	X(METADATA_FS_RMDIR_INCREMENT)              /* Metadata server RMDIR operations */                                \
	X(METADATA_FS_SYMLINK_INCREMENT)            /* Metadata server SYMLINK operations */                              \
	X(METADATA_FS_READLINK_INCREMENT)           /* Metadata server READLINK operations */                             \
	X(METADATA_FS_MKNOD_INCREMENT)              /* Metadata server MKNOD operations */                                \
	X(METADATA_FS_UNLINK_INCREMENT)             /* Metadata server UNLINK operations */                               \
	X(METADATA_FS_RENAME_INCREMENT)             /* Metadata server RENAME operations */                               \
	X(METADATA_FS_LINK_INCREMENT)               /* Metadata server LINK operations */                                 \
	X(METADATA_FS_READDIR_INCREMENT)            /* Metadata server READDIR operations */                              \
	X(METADATA_FS_OPEN_INCREMENT)               /* Metadata server OPEN operations */                                 \
	X(METADATA_FS_READ_INCREMENT)               /* Metadata server READ operations */                                 \
	X(METADATA_FS_WRITE_INCREMENT)              /* Metadata server WRITE operations */

namespace master {
enum class U64 : uint8_t {
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define X(name) name,
    METRIC_METADATA_UINT64_LIST(X)
#undef X
    COUNT
};
}

void increment(master::U64 enu);

void set(master::U64 enu, uint64_t val);

// N.B: This function should be only called from a single thread, never from
// more than one threads (due to the static vector)
MetricSerialized serializeMasterMetrics();

} // metricsNew

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
#ifdef HAVE_PROMETHEUS
#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/registry.h>
#include <prometheus/family.h>

using CounterFamily = prometheus::Family<prometheus::Counter>;

#endif

namespace metrics {

class Counter {
public:
enum class Master : unsigned int {
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

void init(const char* host);
void destroy();

} // metrics

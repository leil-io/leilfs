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

#include "metrics/master.h"
#include "metrics.h"
#include "metrics/utils.h"

#ifdef HAVE_PROMETHEUS
using namespace metrics;

Master::Master(std::shared_ptr<prometheus::Registry> &registry) {
	// clang-format off
	// NOLINTBEGIN(cppcoreguidelines-prefer-member-initializer)
	packetClientCounter = &setupFamily(
		"metadata_observed_packets_client_total",
		"Number of observed packets from and for client", registry);
	byteClientCounter = &setupFamily(
		"metadata_observed_bytes_client_total",
		"Number of observed bytes from and for client", registry);
	filesystemCounter = &setupFamily(
		"metadata_stats_total",
		"Number of observed filesystem operations", registry);
	chunkCounter= &setupFamily(
		"metadata_chunk_operations_total",
		"Number of chunk operations", registry);
	// NOLINTEND(cppcoreguidelines-prefer-member-initializer)

	// A very hacky way to allow compile time checking if metrics have been
	// set. Any enum value not used will throw a compile time error.
	using Master = Counter::Master;
	Master start = Master::KEY_START;
	switch (start) {
		case Master::KEY_START:
			[[fallthrough]];
		case Master::CHUNK_DELETE:
			masterCounters[static_cast<unsigned int>(Master::CHUNK_DELETE)] =
				Counter(
					{{"chunk", "operations"}, {"operation", "delete"}},
					chunkCounter);
			[[fallthrough]];
		case Master::CHUNK_REPLICATE:
			masterCounters[static_cast<unsigned int>(Master::CHUNK_REPLICATE)] =
				Counter(
					{{"chunk", "operations"}, {"operation", "replicate"}},
					chunkCounter);
			[[fallthrough]];
		case Master::FS_STATFS:
			masterCounters[static_cast<unsigned int>(Master::FS_STATFS)] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "STATFS"}},
					filesystemCounter);
			[[fallthrough]];
		case Master::FS_GETATTR:
			masterCounters[static_cast<unsigned int>(Master::FS_GETATTR)] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "GETATTR"}},
					filesystemCounter);
			[[fallthrough]];
		case Master::FS_SETATTR:
			masterCounters[static_cast<unsigned int>(Master::FS_SETATTR)] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "SETATTR"}},
					filesystemCounter);
			[[fallthrough]];
		case Master::FS_LOOKUP:
			masterCounters[static_cast<unsigned int>(Master::FS_LOOKUP)] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "LOOKUP"}},
					filesystemCounter);
			[[fallthrough]];
		case Master::FS_MKDIR:
			masterCounters[static_cast<unsigned int>(Master::FS_MKDIR)] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "MKDIR"}},
					filesystemCounter);
			[[fallthrough]];
		case Master::FS_RMDIR:
			masterCounters[static_cast<unsigned int>(Master::FS_RMDIR)] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "RMDIR"}},
					filesystemCounter);
			[[fallthrough]];
		case Master::FS_SYMLINK:
			masterCounters[static_cast<unsigned int>(Master::FS_SYMLINK)] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "SYMLINK"}},
					filesystemCounter);
			[[fallthrough]];
		case Master::FS_READLINK:
			masterCounters[static_cast<unsigned int>(Master::FS_READLINK)] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "READLINK"}},
					filesystemCounter);
			[[fallthrough]];
		case Master::FS_MKNOD:
			masterCounters[static_cast<unsigned int>(Master::FS_MKNOD)] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "MKNOD"}},
					filesystemCounter);
			[[fallthrough]];
		case Master::FS_UNLINK:
			masterCounters[static_cast<unsigned int>(Master::FS_UNLINK)] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "UNLINK"}},
					filesystemCounter);
			[[fallthrough]];
		case Master::FS_RENAME:
			masterCounters[static_cast<unsigned int>(Master::FS_RENAME)] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "RENAME"}},
					filesystemCounter);
			[[fallthrough]];
		case Master::FS_LINK:
			masterCounters[static_cast<unsigned int>(Master::FS_LINK)] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "LINK"}},
					filesystemCounter);
			[[fallthrough]];
		case Master::FS_READDIR:
			masterCounters[static_cast<unsigned int>(Master::FS_READDIR)] = Counter(
				{{"filesystem", "operations"}, {"operation", "READDIR"}},
				filesystemCounter);
			[[fallthrough]];
		case Master::FS_OPEN:
			masterCounters[static_cast<unsigned int>(Master::FS_OPEN)] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "OPEN"}},
					filesystemCounter);
			[[fallthrough]];
		case Master::FS_READ:
			masterCounters[static_cast<unsigned int>(Master::FS_READ)] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "READ"}},
					filesystemCounter);
			[[fallthrough]];
		case Master::FS_WRITE:
			masterCounters[static_cast<unsigned int>(Master::FS_WRITE)] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "WRITE"}},
					filesystemCounter);
			[[fallthrough]];
		case Master::CLIENT_RX_PACKETS:
			masterCounters[static_cast<unsigned int>(Master::CLIENT_RX_PACKETS)] =
				Counter(
					{{"protocol", "tcp"}, {"direction", "rx"}},
					packetClientCounter);
			[[fallthrough]];
		case Master::CLIENT_TX_PACKETS:
			masterCounters[static_cast<unsigned int>(Master::CLIENT_TX_PACKETS)] =
				Counter(
					{{"protocol", "tcp"}, {"direction", "tx"}},
					packetClientCounter);
			[[fallthrough]];
		case Master::CLIENT_RX_BYTES:
			masterCounters[static_cast<unsigned int>(Master::CLIENT_RX_BYTES)] =
				Counter(
					{{"protocol", "tcp"}, {"direction", "rx"}},
					byteClientCounter);
			[[fallthrough]];
		case Master::CLIENT_TX_BYTES:
			masterCounters[static_cast<unsigned int>(Master::CLIENT_TX_BYTES)] =
				Counter(
					{{"protocol", "tcp"}, {"direction", "tx"}},
					byteClientCounter);
			[[fallthrough]];
		case Master::KEY_END:
			break;
	}
	// clang-format on
}
#endif

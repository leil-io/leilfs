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
	packet_client_counter = &setupFamily(
		"metadata_observed_packets_client_total",
		"Number of observed packets from and for client", registry);
	byte_client_counter = &setupFamily(
		"metadata_observed_bytes_client_total",
		"Number of observed bytes from and for client", registry);
	filesystem_counter = &setupFamily(
		"metadata_stats_total",
		"Number of observed filesystem operations", registry);
	chunkCounter= &setupFamily(
		"metadata_chunk_operations_total",
		"Number of chunk operations", registry);
	// NOLINTEND(cppcoreguidelines-prefer-member-initializer)

	// A very hacky way to allow compile time checking if metrics have been
	// set. Any enum value not used will throw a compile time error.
	using master::Counters;
	Counters start = Counters::KEY_START;
	switch (start) {
		case Counters::KEY_START:
			[[fallthrough]];
		case Counters::CHUNK_DELETE:
			counters[static_cast<unsigned int>(Counters::CHUNK_DELETE)] =
				Counter(
					{{"chunk", "operations"}, {"operation", "delete"}},
					chunkCounter);
			[[fallthrough]];
		case Counters::CHUNK_REPLICATE:
			counters[static_cast<unsigned int>(Counters::CHUNK_REPLICATE)] =
				Counter(
					{{"chunk", "operations"}, {"operation", "replicate"}},
					chunkCounter);
			[[fallthrough]];
		case Counters::FS_STATFS:
			counters[static_cast<unsigned int>(Counters::FS_STATFS)] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "STATFS"}},
					filesystem_counter);
			[[fallthrough]];
		case Counters::FS_GETATTR:
			counters[static_cast<unsigned int>(Counters::FS_GETATTR)] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "GETATTR"}},
					filesystem_counter);
			[[fallthrough]];
		case Counters::FS_SETATTR:
			counters[static_cast<unsigned int>(Counters::FS_SETATTR)] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "SETATTR"}},
					filesystem_counter);
			[[fallthrough]];
		case Counters::FS_LOOKUP:
			counters[static_cast<unsigned int>(Counters::FS_LOOKUP)] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "LOOKUP"}},
					filesystem_counter);
			[[fallthrough]];
		case Counters::FS_MKDIR:
			counters[static_cast<unsigned int>(Counters::FS_MKDIR)] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "MKDIR"}},
					filesystem_counter);
			[[fallthrough]];
		case Counters::FS_RMDIR:
			counters[static_cast<unsigned int>(Counters::FS_RMDIR)] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "RMDIR"}},
					filesystem_counter);
			[[fallthrough]];
		case Counters::FS_SYMLINK:
			counters[static_cast<unsigned int>(Counters::FS_SYMLINK)] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "SYMLINK"}},
					filesystem_counter);
			[[fallthrough]];
		case Counters::FS_READLINK:
			counters[static_cast<unsigned int>(Counters::FS_READLINK)] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "READLINK"}},
					filesystem_counter);
			[[fallthrough]];
		case Counters::FS_MKNOD:
			counters[static_cast<unsigned int>(Counters::FS_MKNOD)] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "MKNOD"}},
					filesystem_counter);
			[[fallthrough]];
		case Counters::FS_UNLINK:
			counters[static_cast<unsigned int>(Counters::FS_UNLINK)] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "UNLINK"}},
					filesystem_counter);
			[[fallthrough]];
		case Counters::FS_RENAME:
			counters[static_cast<unsigned int>(Counters::FS_RENAME)] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "RENAME"}},
					filesystem_counter);
			[[fallthrough]];
		case Counters::FS_LINK:
			counters[static_cast<unsigned int>(Counters::FS_LINK)] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "LINK"}},
					filesystem_counter);
			[[fallthrough]];
		case Counters::FS_READDIR:
			counters[static_cast<unsigned int>(Counters::FS_READDIR)] = Counter(
				{{"filesystem", "operations"}, {"operation", "READDIR"}},
				filesystem_counter);
			[[fallthrough]];
		case Counters::FS_OPEN:
			counters[static_cast<unsigned int>(Counters::FS_OPEN)] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "OPEN"}},
					filesystem_counter);
			[[fallthrough]];
		case Counters::FS_READ:
			counters[static_cast<unsigned int>(Counters::FS_READ)] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "READ"}},
					filesystem_counter);
			[[fallthrough]];
		case Counters::FS_WRITE:
			counters[static_cast<unsigned int>(Counters::FS_WRITE)] =
				Counter(
					{{"filesystem", "operations"}, {"operation", "WRITE"}},
					filesystem_counter);
			[[fallthrough]];
		case Counters::CLIENT_RX_PACKETS:
			counters[static_cast<unsigned int>(Counters::CLIENT_RX_PACKETS)] =
				Counter(
					{{"protocol", "tcp"}, {"direction", "rx"}},
					packet_client_counter);
			[[fallthrough]];
		case Counters::CLIENT_TX_PACKETS:
			counters[static_cast<unsigned int>(Counters::CLIENT_TX_PACKETS)] =
				Counter(
					{{"protocol", "tcp"}, {"direction", "tx"}},
					packet_client_counter);
			[[fallthrough]];
		case Counters::CLIENT_RX_BYTES:
			counters[static_cast<unsigned int>(Counters::CLIENT_RX_BYTES)] =
				Counter(
					{{"protocol", "tcp"}, {"direction", "rx"}},
					byte_client_counter);
			[[fallthrough]];
		case Counters::CLIENT_TX_BYTES:
			counters[static_cast<unsigned int>(Counters::CLIENT_TX_BYTES)] =
				Counter(
					{{"protocol", "tcp"}, {"direction", "tx"}},
					byte_client_counter);
			[[fallthrough]];
		case Counters::KEY_END:
			break;
	}
	// clang-format on
}
#endif

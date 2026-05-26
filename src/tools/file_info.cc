/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2017 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


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

#include "common/platform.h"

#include <stdio.h>

#include "common/chunk_copies_calculator.h"
#include "common/server_connection.h"
#include "common/type_defs.h"
#include "protocol/cltoma.h"
#include "protocol/matocl.h"
#include "tools/tools_commands.h"
#include "tools/tools_common_functions.h"

static int kInfiniteTimeout = 10 * 24 * 3600 * 1000; // simulate infinite timeout (10 days)

static void file_info_usage() {
	fprintf(stderr,
	        "show files info (shows detailed info of each file chunk)\n\nusage:\n"
	        " leil fileinfo name [name ...]\n");
}

static std::string chunkTypeToString(ChunkPartType type) {
	if (slice_traits::isXor(type) || slice_traits::isEC(type)) {
		std::stringstream ss;
		ss << " part " << type.getSlicePart() + 1 << "/" << slice_traits::getNumberOfParts(type)
		   << " of " << to_string(type.getSliceType());
		return ss.str();
	}
	return "";
}

static int chunks_info(const char *file_name, int fd, inode_t inode) {
	static constexpr uint32_t kRequestSize = 100;
	std::vector<ChunkWithAddressAndLabel> chunks;
	uint32_t msgid{0};
	uint32_t chunkIndex{0};

	try {
		do {
			MessageBuffer request, response;
			cltoma::chunksInfo::serialize(request, (uint32_t)0, (uint32_t)0, (uint32_t)0, inode,
			                              chunkIndex, kRequestSize);
			response = ServerConnection::sendAndReceive(
			    fd, request, SAU_MATOCL_CHUNKS_INFO,
			    ServerConnection::ReceiveMode::kReceiveFirstNonNopMessage, kInfiniteTimeout);

			PacketVersion version;
			deserialize(response, version, msgid);

			if (msgid != 0) {
				printf("%s [%" PRIu32 "]: master query: wrong answer (queryid)\n", file_name,
				       chunkIndex);
				return -1;
			}

			uint8_t status = SAUNAFS_STATUS_OK;
			if (version == matocl::chunksInfo::kStatusPacketVersion) {
				matocl::chunksInfo::deserialize(response, msgid, status);
			} else if (version != matocl::chunksInfo::kResponsePacketVersion) {
				printf("%s [%" PRIu32 "]: master query: wrong answer (packet version)\n", file_name,
				       chunkIndex);
				return -1;
			}

			if (status != SAUNAFS_STATUS_OK) {
				printf("%s [%" PRIu32 "]: %s\n", file_name, chunkIndex,
				       saunafs_error_string(status));
				return -1;
			}

			chunks.clear();
			matocl::chunksInfo::deserialize(response, msgid, chunks);

			for (auto &chunk : chunks) {
				if (chunk.chunk_id == 0 && chunk.chunk_version == 0) {
					printf("\tchunk %" PRIu32 ": empty\n", chunkIndex);
				} else {
					printf("\tchunk %" PRIu32 ": %016" PRIX64 "_%08" PRIX32 " / (id:%" PRIu64
					       " ver:%" PRIu32 ")\n",
					       chunkIndex, chunk.chunk_id, chunk.chunk_version, chunk.chunk_id,
					       chunk.chunk_version);

					ChunkCopiesCalculator chunk_calculator;
					for (const auto &part : chunk.chunk_parts) {
						chunk_calculator.addPart(part.chunkType, MediaLabel::kWildcard);
					}
					chunk_calculator.evalRedundancyLevel();

					if (!chunk.chunk_parts.empty()) {
						std::sort(chunk.chunk_parts.begin(), chunk.chunk_parts.end());
						for (size_t i = 0; i < chunk.chunk_parts.size(); ++i) {
							printf("\t\tcopy %zu: %s:%s%s\n", i + 1,
							       chunk.chunk_parts[i].address.toString().c_str(),
							       chunk.chunk_parts[i].label.c_str(),
							       chunkTypeToString(chunk.chunk_parts[i].chunkType).c_str());
						}
					}

					if (chunk_calculator.getFullCopiesCount() == 0) {
						if (chunk.chunk_parts.empty()) {
							printf("\t\tno valid copies !!!\n");
						} else {
							printf("\t\tnot enough parts available\n");
						}
					}
				}
				++chunkIndex;
			}
		} while (chunks.size() >= kRequestSize);

		return 0;
	} catch (const Exception &e) {
		fprintf(stderr, "%s\n", e.what());
		return -1;
	}
}

static int file_info(const char *fileName) {
	std::vector<uint8_t> buffer;
	inode_t inode;
	int fd;

	fd = open_master_conn(fileName, &inode, nullptr, false);
	if (fd < 0) {
		return -1;
	}
	try {
		printf("%s:\n", fileName);

		if (chunks_info(fileName, fd, inode) < 0) {
			close_master_conn(1);
			return -1;
		}
	} catch (IncorrectDeserializationException &e) {
		printf("%s [0]: master query: wrong answer (%s)\n", fileName, e.what());
		close_master_conn(1);
		return -1;
	}
	close_master_conn(0);
	return 0;
}

int file_info_run(int argc, char **argv) {
	int status;

	argc -= optind;
	argv += optind;

	if (argc < 1) {
		file_info_usage();
		return 1;
	}

	status = 0;
	while (argc > 0) {
		if (file_info(*argv) < 0) {
			status = 1;
		}
		argc--;
		argv++;
	}
	return status;
}

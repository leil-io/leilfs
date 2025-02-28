/*
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

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

#include "bgjobs.h"
#include "common/platform.h"

#include <cstdint>
#include <list>
#include <memory>
#include <set>
#include <vector>

#include "chunkserver-common/disk_utils.h"
#include "chunkserver/aligned_allocator.h"
#include "chunkserver/output_buffer.h"
#include "common/chunk_part_type.h"
#include "common/network_address.h"
#include "common/slice_traits.h"
#include "devtools/request_log.h"
#include "protocol/cltocs.h"

using AlignedVectorForIO =
    std::vector<uint8_t, AlignedAllocator<uint8_t, disk::kIoBlockSize>>;

// 4 K + 64 K
// [4K    ....   HEADER]+[Up to SFSBLOCKSIZE of aligned data              ...]
constexpr uint32_t kIOAlignedPacketSize = disk::kIoBlockSize + SFSBLOCKSIZE;

// Starting point to have the actual data aligned to 4 K
constexpr uint32_t kIOAlignedOffset =
    disk::kIoBlockSize - cltocs::writeData::kPrefixSize;

// Alias for better readability
#define kInvalidPacket nullptr

/**
 * @brief Encapsulates the data associated with a packet.
 *
 * Including pointers to the packet data, the number of bytes left to process,
 * and an optional output buffer for writing data.
 */
struct PacketStruct {
	uint8_t *startPtr = nullptr;
	uint32_t bytesLeft = 0;
	std::vector<uint8_t> packet;

	/// Aligned buffer for write operations.
	/// This buffer is used to ensure that the data is aligned to the disk block
	/// size (4 KiB). This is needed if DirectIO is required.
	AlignedVectorForIO alignedBuffer;
	/// Flag indicating whether the aligned buffer is in use.
	bool useAlignedMemory = false;

	std::shared_ptr<OutputBuffer> outputBuffer;
};

class MessageSerializer;

/**
 * @brief Represents a single connection to a chunkserver.
 *
 * This struct manages the state and data associated with a connection to a
 * chunkserver. It includes information about the connection's state, mode,
 * sockets, and various buffers used for reading and writing data. It also
 * maintains metadata for managing the connection's lifecycle and handling
 * retries, timeouts, and partial writes.
 *
 * @details
 * The `ChunkserverEntry` struct is used extensively within the
 * `NetworkWorkerThread` to manage connections. It supports both reading and
 * writing operations, including forwarding data to other chunkservers in a
 * write chain. The struct also tracks job IDs and partially completed writes to
 * ensure data consistency and proper error handling.
 */
struct ChunkserverEntry {
	/// The possible modes of a `ChunkserverEntry`.
	enum class Mode : uint8_t {
		Header,  // reading packet header
		Data     // reading packet data
	};

	/// The possible connection states of a `ChunkserverEntry`.
	enum class State : uint8_t {
		Idle,        // idle connection, new or used previously
		Read,        // after CLTOCS_READ, but didn't send all of the
		             // CSTOCL_READ_(DATA|STATUS)
		GetBlock,    // after CSTOCS_GET_CHUNK_BLOCKS, but didn't send response
		WriteLast,   // ready for writing data; data not forwarded to other CSs
		Connecting,  // connecting to other chunkserver to form a writing chain
		WriteInit,   // sending packet forming a chain to the next chunkserver
		WriteForward,  // ready for writing data; will be forwarded to other CSs
		WriteFinish,   // write error, will be closed after sending error status
		Close,         // close request, will change to CloseWait or Closed
		CloseWait,  // waits for a worker to finish a job, then will be Closed
		Closed      // ready to be deleted
	};

	// Some constants to improve readability
	static constexpr int kInvalidSocket = -1;
	static constexpr int kInitConnectionOK = 0;
	static constexpr int kInitConnectionFailed = -1;
	static constexpr uint32_t kGenerateChartExpectedPacketSize =
	    sizeof(uint32_t);

	JobPool *workerJobPool;  // Job pool assigned to a given network worker thread

	ChunkserverEntry::State state = ChunkserverEntry::State::Idle;
	ChunkserverEntry::Mode mode = ChunkserverEntry::Mode::Header;
	ChunkserverEntry::Mode fwdMode = ChunkserverEntry::Mode::Header;

	int sock;
	int fwdSocket = kInvalidSocket; ///< forwarding socket for writing
	uint64_t connectStartTimeUSec = 0; ///< for timeout and retry (usec)
	uint8_t connectRetryCounter = 0; ///< for timeout and retry
	NetworkAddress fwdServer; // the next server in write chain
	int32_t pDescPos = -1;  ///< Position in the poll descriptors array
	int32_t fwdPDescPos = -1;  ///< Position in poll descriptors for fwdSocket
	uint32_t lastActivity = 0; ///< Last activity time
	uint8_t headerBuffer[PacketHeader::kSize]{};  ///< buffer for packet header
	uint8_t fwdHeaderBuffer[PacketHeader::kSize]{};  ///< fwd packet header buff
	/// Stores the data of the incoming packet for processing
	PacketStruct inputPacket;
	uint8_t *fwdStartPtr = nullptr; ///< used for forwarding inputpacket data
	uint32_t fwdBytesLeft = 0; ///< used for forwarding inputpacket data
	PacketStruct fwdInputPacket; ///< used for receiving status from fwdSocket
	std::vector<uint8_t> fwdInitPacket; ///< used only for write initialization

	/// List of output packets waiting to be sent to the clients
	std::list<std::unique_ptr<PacketStruct>> outputPackets;

	/* write */
	uint32_t writeJobId = 0; ///< ID of the current write job being processed
	uint32_t writeJobWriteId = 0; ///< Specific write operation from client
	/// writeJobWriteId's which:
	/// - have been completed by our worker, but need ack from the next
	///   chunkserver from the chain.
	/// - have been acked by the next chunkserver from the chain, but are still
	///   being written by us.
	std::set<uint32_t> partiallyCompletedWrites;

	/* read */
	uint16_t maxParallelHddReadJobs; ///< Maximum size of pendingReadDataPackets.

	/// List of read data packets waiting for the HDD worker to finish, and then be sent.
	std::list<std::unique_ptr<PacketStruct>> pendingReadDataPackets;
	std::list<uint32_t> pendingReadJobIds; ///< Job IDs for pending read operations.
	/// List of read data packets within a failing read operation, which are to be discarded.
	std::list<std::unique_ptr<PacketStruct>> toDiscardReadDataPackets;
	std::list<uint32_t> toDiscardReadJobIds; ///< Job IDs for read operations to discard.

	size_t readyReadDataPackets{0}; ///< Number of pending read data packets ready to be sent.

	/* get blocks */
	uint32_t getBlocksJobId = 0; ///< Current job ID for retrieving chunk blocks
	uint16_t getBlocksJobResult = 0; ///< Result of the get blocks job

	/* PacketStruct is common for read and write but meaning is different !!! */
	std::unique_ptr<PacketStruct> writePacket =
	    std::make_unique<PacketStruct>();

	uint8_t isChunkOpen = 0;
	uint64_t chunkId = 0; // R+W
	uint32_t chunkVersion = 0; // R+W
	ChunkPartType chunkType = slice_traits::standard::ChunkPartType(); // R
	uint32_t offset = 0; ///< R: Offset within the chunk for the operation.
	uint32_t size = 0; ///< R: Size of the current operation.

	/// Pointer to the concrete serializer singleton.
	/// Serializers could be of type:
	/// - LegacyMessageSerializer: for legacy messages
	/// - SaunaFsMessageSerializer: for new messages
	MessageSerializer* messageSerializer = nullptr; // R+W

	LOG_AVG_TYPE readOperationTimer;

	ChunkserverEntry(int socket, JobPool *workerJobPool, uint16_t maxParallelHddReadJobs)
	    : workerJobPool(workerJobPool), sock(socket),
	      maxParallelHddReadJobs(maxParallelHddReadJobs) {
		inputPacket.bytesLeft = PacketHeader::kSize;
		inputPacket.startPtr = headerBuffer;
	}

	// Disallow copying and moving to avoid misuse.
	ChunkserverEntry(const ChunkserverEntry &) = delete;
	ChunkserverEntry &operator=(const ChunkserverEntry &) = delete;
	ChunkserverEntry(ChunkserverEntry &&) = delete;
	ChunkserverEntry &operator=(ChunkserverEntry &&) = delete;

	/// Destructor: closes the sockets.
	~ChunkserverEntry();

	/// Attaches a packet to the output packet list (taking ownership).
	inline void attachPacket(std::unique_ptr<PacketStruct> &&packet);

	/// Preserves the inputPacket buffer into writePacket (to avoid copying it).
	/// Used for write operations, where the data comes from the network.
	inline void preserveInputPacket();

	/// Creates an attached packet from the given vector.
	/// The function takes ownership of the vector.
	void createAttachedPacket(std::vector<uint8_t> &packet);

	/// Creates an attached packet with the given type and operation size.
	///
	/// @param type The type of the packet.
	/// @param operationSize The size of the operation.
	/// @return Pointer to the created packet data.
	uint8_t *createAttachedPacket(uint32_t type, uint32_t operationSize);

	/// Creates a detached packet with an output buffer.
	/// @see OutputBufferPool
	static std::unique_ptr<PacketStruct> createDetachedPacketWithOutputBuffer(
	    const std::vector<uint8_t> &packetPrefix);

	/// Handles forwarding errors by setting the appropriate error status and
	/// transitioning the connection state to `WriteFinish`.
	///
	/// This function is called when an error occurs during forwarding
	/// operations, such as read or write errors on the forwarding socket. It
	/// serializes an error status message and attaches it to the packet, then
	/// sets the state to `WriteFinish` to indicate that the connection should
	/// be closed after sending the error status.
	void fwdError();

	/// Handles the event when a connection to another chunkserver is
	/// successfully established.
	///
	/// This function is called when the connection to the next chunkserver in
	/// the write chain is successfully established.
	///
	/// Typically invoked after a successful non-blocking connect operation.
	///
	/// \see ChunkserverEntry::retryConnect
	void fwdConnected();

	/// Reads data from the forwarding socket and processes it.
	void fwdRead();

	/// Writes data to the forwarding socket.
	///
	/// This function handles writing data to the forwarding socket
	/// (`fwdSocket`). It attempts to write the remaining data in the
	/// `fwdStartPtr` buffer to the socket.
	///
	/// This function is typically invoked when the forwarding socket is ready
	/// for writing, as indicated by the `POLLOUT` event in the poll descriptor.
	void fwdWrite();

	/// Initiates the forwarding process for the current packet.
	///
	/// This function is responsible for initiating the forwarding process of
	/// the current packet to the next chunkserver in the chain.
	///
	/// This function is typically called when a packet needs to be forwarded to
	/// another chunkserver for further processing.
	void forward();

	/// Initializes the connection to the next chunkserver in the chain.
	///
	/// This function sets up the necessary parameters and state for
	/// establishing a connection to the next chunkserver.
	///
	/// This function is typically called when a new connection needs to be made
	/// to forward data to another chunkserver.
	///
	/// @return An integer status code indicating the success or failure of the
	///         connection initialization. A return value of 0 indicates
	///         success, while a non-zero value indicates an error.
	int initConnection();

	/// Attempts to re-establish a connection to the next chunkserver.
	/// Implements a retry mechanism to ensure that the connection
	/// is eventually established
	void retryConnect();

	/// Checks and processes the next packet in the input buffer.
	void checkNextPacket();

	/// Processes a received packet based on its type.
	///
	/// @param type The type of the packet.
	/// @param data Pointer to the packet data.
	/// @param length The length of the packet data.
	void gotPacket(uint32_t type, const uint8_t *data, uint32_t length);

	/* IDLE Operations */

	/// Answers to a ping message with the given data and length.
	void ping(const uint8_t *data, PacketHeader::Length length);

	/// Initializes a read operation
	///
	/// @param data Pointer to the buffer containing the information to read.
	/// @param type The type of the packet.
	/// @param length The length of the packet data.
	void readInit(const uint8_t *data, PacketHeader::Type type,
	              PacketHeader::Length length);

	/// Continues a previously started read operation.
	///
	/// Processes the remaining data to be read from the chunkserver. If all
	/// data has been read, it sends a read status message and closes the chunk.
	/// Otherwise, it prepares the next part of the read operation.
	///
	/// @see ChunkserverEntry::readInit
	void readContinue();

	/// Requests a data prefetch operation.
	/// Prefetch in this context means reading data from the disk and storing it
	/// in the page cache.
	void prefetch(const uint8_t *data, PacketHeader::Type type,
	              PacketHeader::Length length);
	
	/// Prepares the discard of the current ongoing read operations.
	///
	/// It disables the jobs, changes the callback and moves the jobs from pending
	/// to discard lists.
	void prepareDiscardReadJobs();

	/// Callback for when a read operation finishes.
	static void readFinishedCallback(uint8_t status, void *entry);
	/// Callback for when a discarded read operation finishes.
	static void readDiscardCallback(uint8_t status, void *entry);
	/// Callback after delayed close operations.
	static void delayedCloseCallback(uint8_t status, void *entry);
	/// Callback for when a write operation finishes.
	static void writeFinishedCallback(uint8_t status, void *entry);
	/// Callback for legacy chunk block retrieval completion.
	static void sauGetChunkBlocksFinishedLegacyCallback(uint8_t status,
	                                                    void *entry);
	/// Callback for chunk block retrieval completion.
	static void sauGetChunkBlocksFinishedCallback(uint8_t status, void *entry);
	/// Callback for chunk block retrieval completion.
	static void getChunkBlocksFinishedCallback(uint8_t status, void *entry);

	/// Retrieves chunk blocks from the given information.
	void getChunkBlocks(const uint8_t *data, uint32_t length);

	/// Retrieves chunk blocks from the given information using the new way.
	void sauGetChunkBlocks(const uint8_t *data, uint32_t length);

	/// Retrieves the list with the HDDs information.
	void hddListV2([[maybe_unused]] const uint8_t *data, uint32_t length);

	/// Lists the disk groups (if the DiskManager supports it).
	void listDiskGroups([[maybe_unused]] const uint8_t *data,
	                    [[maybe_unused]] uint32_t length);

	/// Generates a chart in PNG or CSV format.
	void generateChartPNGorCSV(const uint8_t *data, uint32_t length);

	/// Generates chart data.
	void generateChartData(const uint8_t *data, uint32_t length);

	/// Adds a chunk to the test queue for CRC checking.
	/// Usually the master server sends this command after a client reports an
	/// error in the CRC of a block.
	void testChunk(const uint8_t *data, uint32_t length);

	/// Initializes a write operation.
	void writeInit(const uint8_t *data, PacketHeader::Type type,
	               PacketHeader::Length length);

	/* WriteLast or WriteForward*/

	/// Writes a block of data to the drives.
	void writeData(const uint8_t *data, PacketHeader::Type type,
	               PacketHeader::Length length);

	/// Finalizes a write operation and closes the chunk and connection.
	void writeEnd(const uint8_t *data, uint32_t length);

	/// Posts a write a status message to be sent through the network.
	void writeStatus(const uint8_t *data, PacketHeader::Type type,
	                 PacketHeader::Length length);

	/* servePoll related */

	/// Writes data from an output packet to the socket.
	void writeToSocket();

	/// Reads data from the socket into the input buffer.
	void readFromSocket();
	/// Checks if it is a read operation and tries to finish it.
	void outputCheckReadFinished();

	/// Closes all active jobs and updates the state.
	///
	/// This function disables and changes the callback for any active read,
	/// write, or get blocks jobs. If no jobs are active, it closes the chunk
	/// and sets the state to `Closed`.
	///
	/// Called from the `NetworkWorkerThread` when a connection is closed.
	void closeJobs();
};

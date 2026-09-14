/*
   Copyright 2013-2014 EditShare
   Copyright 2013-2017 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

   This file is part of LeilFS.

   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/platform.h"

#include "common/chunk_part_type.h"
#include "common/exception.h"
#include "errors/sfserr.h"
#include "common/network_address.h"

SAUNAFS_CREATE_EXCEPTION_CLASS(ConfigurationException, Exception);
SAUNAFS_CREATE_EXCEPTION_CLASS(FilesystemException, Exception);
SAUNAFS_CREATE_EXCEPTION_CLASS(InitializeException, Exception);
SAUNAFS_CREATE_EXCEPTION_CLASS(ConnectionException, Exception);

SAUNAFS_CREATE_EXCEPTION_CLASS(ReadException, Exception);
SAUNAFS_CREATE_EXCEPTION_CLASS(RecoverableReadException, ReadException);
SAUNAFS_CREATE_EXCEPTION_CLASS(UnrecoverableReadException, ReadException);
SAUNAFS_CREATE_EXCEPTION_CLASS(NoValidCopiesReadException, RecoverableReadException);

SAUNAFS_CREATE_EXCEPTION_CLASS(WriteException, Exception);
SAUNAFS_CREATE_EXCEPTION_CLASS(RecoverableWriteException, WriteException);
SAUNAFS_CREATE_EXCEPTION_CLASS(UnrecoverableWriteException, WriteException);
SAUNAFS_CREATE_EXCEPTION_CLASS(NoValidCopiesWriteException, RecoverableWriteException);

class ChunkCrcException : public RecoverableReadException {
public:
	ChunkCrcException(const std::string& message, const NetworkAddress& server,
			const ChunkPartType& chunkType)
			: RecoverableReadException(message + " (server " + server.toString() + ")"),
			  server_(server), chunkType_(chunkType) {
	}

	~ChunkCrcException() noexcept {
	}

	const NetworkAddress& server() const { return server_; }
	const ChunkPartType& chunkType() const { return chunkType_; }

private:
	NetworkAddress server_;
	ChunkPartType chunkType_;
};

class ParseException : public Exception {
public:
	ParseException(const std::string& message)
			: Exception(message) {
	}

	ParseException(int line, const std::string& message)
			: Exception("line " + std::to_string(line) + " : " + message) {
	}

	~ParseException() noexcept {
	}
};

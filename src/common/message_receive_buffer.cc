/*
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
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

#include "common/platform.h"
#include "common/message_receive_buffer.h"
#include "common/sockets.h"

#include <unistd.h>
#include <cstring>

ssize_t MessageReceiveBuffer::readFrom(int fd) {
	sassert(bytesReceived_ < buffer_.size());
	int ret = tcprecv(fd, buffer_.data() + bytesReceived_, buffer_.size() - bytesReceived_);
	if (ret < 0) {
		return ret;
	}
	bytesReceived_ += ret;
	return ret;
}

void MessageReceiveBuffer::removeMessage() {
	sassert(hasMessageData());
	size_t totalMessageSize = PacketHeader::kSize + getMessageHeader().length;
	if (bytesReceived_ > totalMessageSize) {
		size_t extraDataSize = bytesReceived_ - totalMessageSize;
		memmove(buffer_.data(), buffer_.data() + totalMessageSize, extraDataSize);
	}
	bytesReceived_ -= totalMessageSize;
}

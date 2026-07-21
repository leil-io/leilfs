/*
   Copyright 2025      Leil Storage OÜ

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"

#include "master/filesystem_operation_context.h"

#include "kv/itransaction.h"

FilesystemOperationContext::FilesystemOperationContext(
    std::unique_ptr<kv::IReadOnlyTransaction> txn)
    : roTransaction_(std::move(txn)), transactionType_(TransactionType::kReadOnly) {}

FilesystemOperationContext::FilesystemOperationContext(
    std::unique_ptr<kv::IReadWriteTransaction> txn)
    : rwTransaction_(std::move(txn)), transactionType_(TransactionType::kReadWrite) {}

bool FilesystemOperationContext::hasTransaction() const {
	return rwTransaction_ != nullptr || roTransaction_ != nullptr;
}

bool FilesystemOperationContext::hasReadWriteTransaction() const {
	return rwTransaction_ != nullptr;
}

kv::IReadOnlyTransaction *FilesystemOperationContext::getReadOnlyTransaction() const {
	if (rwTransaction_) {
		// Read-write transaction can be used as read-only
		return rwTransaction_.get();
	}

	return roTransaction_.get();
}

kv::IReadWriteTransaction *FilesystemOperationContext::getReadWriteTransaction() const {
	return rwTransaction_.get();
}

std::unique_ptr<kv::IReadWriteTransaction>
FilesystemOperationContext::releaseReadWriteTransaction() {
	if (rwTransaction_ != nullptr) { transactionType_ = TransactionType::kNone; }
	return std::move(rwTransaction_);
}

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

#include <stdexcept>

#include "common/test_commit_pause.h"
#include "kv/itransaction.h"

FilesystemOperationContext::FilesystemOperationContext(
    std::unique_ptr<kv::IReadOnlyTransaction> txn)
    : roTransaction_(std::move(txn)), transactionType_(TransactionType::kReadOnly) {}

FilesystemOperationContext::FilesystemOperationContext(
    std::unique_ptr<kv::IReadWriteTransaction> txn)
    : rwTransaction_(std::move(txn)), transactionType_(TransactionType::kReadWrite) {}

FilesystemOperationContext::FilesystemOperationContext(
    FilesystemOperationContext &&other) noexcept = default;

FilesystemOperationContext &FilesystemOperationContext::operator=(
    FilesystemOperationContext &&other) noexcept {
	if (this == &other) { return *this; }
	if (transactionEffects_ != nullptr && !transactionEffectsFinished_) {
		transactionEffects_->finish(FilesystemTransactionOutcome::kAborted);
	}
	assert(deferredChangelogEntries_.empty() || !commitConfirmed_);

	rwTransaction_ = std::move(other.rwTransaction_);
	roTransaction_ = std::move(other.roTransaction_);
	transactionType_ = other.transactionType_;
	nodeArena_ = std::move(other.nodeArena_);
	bucketRowCache_ = std::move(other.bucketRowCache_);
	deferChangelog_ = other.deferChangelog_;
	deferredChangelogEntries_ = std::move(other.deferredChangelogEntries_);
	groupCommitBatch_ = other.groupCommitBatch_;
	commitConfirmed_ = other.commitConfirmed_;
	transactionEffects_ = std::move(other.transactionEffects_);
	transactionEffectsFinished_ = other.transactionEffectsFinished_;

	other.transactionType_ = TransactionType::kNone;
	other.deferChangelog_ = false;
	other.groupCommitBatch_ = false;
	other.commitConfirmed_ = false;
	other.transactionEffectsFinished_ = true;
	return *this;
}

FilesystemOperationContext::~FilesystemOperationContext() {
	if (transactionEffects_ != nullptr && !transactionEffectsFinished_) {
		transactionEffects_->finish(FilesystemTransactionOutcome::kAborted);
	}
	assert(deferredChangelogEntries_.empty() || !commitConfirmed_);
}

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

void FilesystemOperationContext::setTransactionEffects(
    std::unique_ptr<FilesystemTransactionEffects> effects) const {
	if (effects == nullptr) { throw std::invalid_argument("transaction effects must not be null"); }
	if (transactionEffects_ != nullptr || transactionEffectsFinished_) {
		throw std::logic_error("transaction effects already attached or finished");
	}
	transactionEffects_ = std::move(effects);
}

void FilesystemOperationContext::finishTransactionEffects(
    FilesystemTransactionOutcome outcome) const noexcept {
	if (transactionEffectsFinished_) { return; }
	if (transactionEffects_ != nullptr) {
		// The H11 window: the commit is durable and its effects have not run. Every commit
		// path, client batch, synchronous op or internal sweep, passes through here.
		if (outcome == FilesystemTransactionOutcome::kCommitted) {
			test_commit_pause::holdAfterDurableCommit(
			    transactionEffects_->retiresChunks() ? "retirement" : "effects");
		}
		transactionEffects_->finish(outcome);
	}
	transactionEffectsFinished_ = true;
}

bool FilesystemOperationContext::commitTransaction() const {
	if (rwTransaction_ == nullptr) { return true; }
	const bool committed = rwTransaction_->commit();
	finishTransactionEffectsAfterCommit(committed);
	return committed;
}

void FilesystemOperationContext::finishTransactionEffectsAfterCommit(
    bool committed) const noexcept {
	if (committed) {
		finishTransactionEffects(FilesystemTransactionOutcome::kCommitted);
	} else if (rwTransaction_ != nullptr && rwTransaction_->commitOutcomeUnknown()) {
		finishTransactionEffects(FilesystemTransactionOutcome::kIndeterminate);
	} else {
		finishTransactionEffects(FilesystemTransactionOutcome::kAborted);
	}
}

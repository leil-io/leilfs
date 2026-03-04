/*
   Copyright 2023      Leil Storage OÜ

   This file is part of SaunaFS.

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

#include "common/platform.h"

#include "fdb/fdb.h"

#include <cstdint>

#include <foundationdb/fdb_c_types.h>

#include "fdb/fdb_future.h"
#include "kv/kv_utils.h"
#include "slogger/slogger.h"

namespace fdb {

const uint8_t *toU8(std::string_view str) {
	return reinterpret_cast<const uint8_t *>(str.data());
}

//Static

fdb_error_t DB::selectAPIVersion(int version) { return fdb_select_api_version(version); }

std::string_view DB::errorMsg(fdb_error_t code) { return fdb_get_error(code); }

bool DB::evaluatePredicate(int predicate_test, fdb_error_t code) {
	return static_cast<bool>(fdb_error_predicate(predicate_test, code));
}

// network

fdb_error_t DB::setNetworkOption(FDBNetworkOption option, std::string_view value /* = {} */) {
	return fdb_network_set_option(option, toU8(value), static_cast<int>(value.length()));
}

fdb_error_t DB::setupNetwork() { return fdb_setup_network(); }

fdb_error_t DB::runNetwork() { return fdb_run_network(); }

fdb_error_t DB::stopNetwork() { return fdb_stop_network(); }

// DB

fdb_error_t DB::setOption(FDBDatabaseOption option, std::string_view value) {
	return fdb_database_set_option(db_.get(), option, toU8(value),
	                               static_cast<int>(value.length()));
}

// Transaction

fdb_error_t Transaction::setOption(FDBTransactionOption option, std::string_view value) {
	return fdb_transaction_set_option(tr_.get(), option, toU8(value),
	                                  static_cast<int>(value.length()));
}

std::optional<kv::Value> Transaction::get(const kv::Key &key, bool snapshot) {
	if (!tr_) { return std::nullopt; }

	FDBFuture *future =
	    fdb_transaction_get(tr_.get(), key.data(), static_cast<int>(key.size()), snapshot);

	error_ = fdb_future_block_until_ready(future);

	if (error_ != 0) {
		safs::log_err("Transaction::get: fdb_future_block_until_ready: error: {}",
		              fdb_get_error(error_));
		return std::nullopt;
	}
	
	fdb_bool_t valuePresent{};
	const uint8_t *valueRead{};
	int valueLength{};

	error_ = fdb_future_get_value(future, &valuePresent, &valueRead, &valueLength);

	if (error_ != 0) {
		safs::log_err("Transaction::get: fdb_future_get_value: error: {}", fdb_get_error(error_));
		fdb_future_destroy(future);
		return std::nullopt;
	}

	kv::Value value;

	if (valuePresent != 0) {
		value.assign(valueRead, valueRead + valueLength);
	} else {
		safs::log_info("Transaction::get: key not found: {}", kv::keyToEscapedAscii(key));
		value.clear();
		return std::nullopt;
	}

	return value;
}

std::unique_ptr<kv::IFuture> Transaction::getAsync(const kv::Key &key, bool snapshot) {
	if (!tr_) { return nullptr; }

	FDBFuture *future = fdb_transaction_get(tr_.get(), key.data(), static_cast<int>(key.size()),
	                                         static_cast<fdb_bool_t>(snapshot));

	return std::make_unique<FDBFutureValue>(future);
}

kv::GetRangeResult Transaction::getRange(
    const kv::KeySelector &begin, const kv::KeySelector &end, int limit, int iteration /* = 0 */,
    bool snapshot /* = false */, bool reverse /* = false */,
    FDBStreamingMode streamingMode /* = FDB_STREAMING_MODE_SERIAL */) {
	if (!tr_) { return {{}, false}; }

	static constexpr int kBytesLimit = 0;

	// For begin selectors: orEqual=0 means >= (inclusive), orEqual=1 means > (exclusive)
	const fdb_bool_t beginOrEqual = begin.isInclusive() ? 0 : 1;
	// FDB offsets are 1-based.
	const int beginOffset = 1 + begin.getOffset();

	// For end selectors (which define an exclusive boundary in FDB):
	// - To include end key in results: use orEqual=1
	// - To exclude end key from results: use orEqual=0
	const fdb_bool_t endOrEqual = end.isInclusive() ? 1 : 0;
	// FDB offsets are 1-based.
	const int endOffset = 1 + end.getOffset();

	FDBFuture *future = fdb_transaction_get_range(
	    tr_.get(), begin.getKey().data(), static_cast<int>(begin.getKey().size()), beginOrEqual,
	    beginOffset, end.getKey().data(), static_cast<int>(end.getKey().size()), endOrEqual,
	    endOffset, limit, kBytesLimit, streamingMode, iteration, static_cast<fdb_bool_t>(snapshot),
	    static_cast<fdb_bool_t>(reverse));

	auto error = fdb_future_block_until_ready(future);

	if (error != 0) {
		safs::log_err("Transaction::getRange: fdb_future_block_until_ready: error: {}",
		              fdb_get_error(error));
		fdb_future_destroy(future);
		return {{}, false};
	}

	const FDBKeyValue *keyValues;
	int count = 0;
	fdb_bool_t more = 0;

	error = fdb_future_get_keyvalue_array(future, &keyValues, &count, &more);

	if (error != 0) {
		safs::log_err("Transaction::getRange: fdb_future_get_keyvalue_array: error: {}",
		              fdb_get_error(error));
		fdb_future_destroy(future);
		return {{}, false};
	}

	std::vector<kv::KeyValuePair> pairs;
	pairs.reserve(count);

	for (int i = 0; i < count; ++i) {
		kv::Key key(keyValues[i].key, keyValues[i].key + keyValues[i].key_length);
		kv::Value value(keyValues[i].value, keyValues[i].value + keyValues[i].value_length);
		pairs.emplace_back(std::move(key), std::move(value));
	}

	fdb_future_destroy(future);

	if (pairs.empty()) {
		safs::log_info("Transaction::getRange: no keys found in range: {} - {}",
		               kv::keyToEscapedAscii(begin.getKey()), kv::keyToEscapedAscii(end.getKey()));
		return {{}, false};
	}

	kv::GetRangeResult result(std::move(pairs), more != 0);

	return result;
}

void Transaction::set(const kv::Key &key, const kv::Value &value) {
	if (!tr_) { return; }

	fdb_transaction_set(tr_.get(), key.data(), static_cast<int>(key.size()), value.data(),
	                    static_cast<int>(value.size()));
}

void Transaction::atomicAdd(const kv::Key &key, const kv::Value &delta) {
	if (!tr_) { return; }

	fdb_transaction_atomic_op(tr_.get(), key.data(), static_cast<int>(key.size()), delta.data(),
	                          static_cast<int>(delta.size()), FDB_MUTATION_TYPE_ADD);
}

void Transaction::remove(const kv::Key &key) {
	if (!tr_) { return; }

	fdb_transaction_clear(tr_.get(), key.data(), static_cast<int>(key.size()));
}

void Transaction::removeRange(const kv::Key &start, const kv::Key &end) {
	if (!tr_) { return; }

	fdb_transaction_clear_range(tr_.get(), start.data(), static_cast<int>(start.size()), end.data(),
	                            static_cast<int>(end.size()));
}

bool Transaction::commit() {
	if (!tr_) { return false; }

	FDBFuture *future = fdb_transaction_commit(tr_.get());

	error_ = fdb_future_block_until_ready(future);

	if (error_ != 0) {
		safs::log_err("Transaction::commit: error: {}", fdb_get_error(error_));
		fdb_future_destroy(future);
		return false;
	}

	int64_t version{};
	fdb_error_t versionError = fdb_transaction_get_committed_version(tr_.get(), &version);

	if (versionError != 0) {
		safs::log_err("Transaction::commit: fdb_transaction_get_committed_version: error: {}",
		              fdb_get_error(versionError));
		fdb_future_destroy(future);
		return false;
	}

	committedVersion_ = version;

	fdb_future_destroy(future);

	return true;
}

}  // namespace fdb

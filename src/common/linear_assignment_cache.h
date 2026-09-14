/*
   Copyright 2023 Leil Storage OÜ

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

#include <algorithm>
#include <array>

#include "common/goal.h"

/**
 * @brief Cache for linear assignment optimizer input/output.
 *
 * Note this cache can be used in multiple scenarios, and the output for the
 * same input can differ for different scenarios.
 *
 * Implementation allows:
 *   - query (```checkMatchAndGetResult```) for the assignment calculated by the
 * ```auctionOptimization``` function given a pair of target and source slice.
 * If no cached entry matches input, then no assignment is replied.
 *   - ```store``` the assignment calculated by the ```auctionOptimization```
 * function and the input that generated it.
 *
 * The cost of both functions is proportional to the sum of the number of parts
 * in the two slices and the sum of the number of labels with copies for each of
 * those parts.
 *
 * Polynomial hashing on the concatenation of both slices is used to find an
 * integer that identifies the input. This values is then used to fast index in
 * the entries container and query/store data.
 */
class LinearAssignmentCache {
public:
	static constexpr LinearAssignmentCache *kNotProvidedCache = nullptr;

	// Single linear optimizer input/output pair.
	class OptimizerInputOutput {
	public:
		OptimizerInputOutput()
		    : target_slice_(Goal::Slice::Type(1)),
		      source_slice_(Goal::Slice::Type(1)),
		      assignment_() {}

		static bool partsMatch(const detail::Slice::PartProxy &part1,
		                       const detail::Slice::PartProxy &part2) {
			return part1.size() == part2.size() &&
			       std::equal(part1.begin(), part1.end(), part2.begin());
		}

		template <std::size_t N>
		bool checkMatchAndGetResult(Goal::Slice &target_slice,
		                            Goal::Slice &source_slice,
		                            std::array<int, N> &assignment) {
			if (target_slice.getType() != target_slice_.getType() ||
			    source_slice.getType() != source_slice_.getType()) {
				return false;
			}
			assert(target_slice.size() == target_slice_.size());
			assert(source_slice.size() == source_slice_.size());

			for (int i = 0; i < target_slice.size(); ++i) {
				if (!partsMatch(target_slice[i], target_slice_[i])) {
					return false;
				}
			}

			for (int i = 0; i < source_slice.size(); ++i) {
				if (!partsMatch(source_slice[i], source_slice_[i])) {
					return false;
				}
			}

			// match is good
			std::copy(assignment_.begin(), assignment_.end(),
			          assignment.begin());

			return true;
		}

		template <std::size_t N>
		void store(Goal::Slice &target_slice, Goal::Slice &source_slice,
		           std::array<int, N> &assignment) {
			target_slice_ = target_slice;
			source_slice_ = source_slice;
			assignment_.assign(assignment.begin(), assignment.end());
		}

	private:
		Goal::Slice target_slice_, source_slice_;
		std::vector<int> assignment_;
	};

	using HashType = int64_t;
	using EntriesContainer = std::unordered_map<HashType, OptimizerInputOutput>;

	LinearAssignmentCache() : cachedData_() {}

	static HashType getLabelValue(MediaLabel label) {
		return static_cast<MediaLabelManager::HandleValue>(label);
	}

	/**
	 * @brief Calculate hash of a part.
	 *
	 * Hash of a part is P(```kBasePart_```) mod ```kModPart_```. P is a
	 * polynomial whose coefficients are values dependent on each (labelId,
	 * copies) pair of the part.
	 *
	 * @param part Part to be calculated hash.
	 * @return Hash of the given part.
	 */
	static HashType hashPart(const detail::Slice::PartProxy &part) {
		HashType hash = 1;
		for (const auto &[labelId, copies] : part) {
			HashType label_hash =
			    (getLabelValue(labelId) * Goal::kMaxExpectedCopies + copies) %
			    kBasePart_;
			hash = (hash * kBasePart_ + label_hash) % kModPart_;
		}
		return hash;
	}

	/**
	 * @brief Calculate hash of a slice starting from a given hash.
	 *
	 * Hash of a part is Q(```kBaseSlice_```) mod ```kModSlice_```. Q is a
	 * polynomial whose coefficients are the hashed of each of the parts in the
	 * slice.
	 *
	 * @param startingHash Could be the ```kInitialStartingHash_``` or some hash
	 * calculation from a previous slice.
	 * @param slice Slice to be calculated hash.
	 * @return Hash of the given slice starting from the provided hash.
	 */
	static HashType hashSlice(HashType startingHash, Goal::Slice &slice) {
		HashType hash = startingHash;
		for (const auto &part : slice) {
			HashType part_value = hashPart(part);
			hash = (hash * kBaseSlice_ + part_value) % kModSlice_;
		}
		return hash;
	}

	/**
	 * @brief Calculate the hash of a concatenation of slices.
	 *
	 * Note the order is important.
	 */
	static HashType hashSlicePair(Goal::Slice &target_slice,
	                              Goal::Slice &source_slice) {
		HashType hash = hashSlice(kInitialStartingHash_, target_slice);
		hash = hashSlice(hash, source_slice);
		return hash;
	}

	template <std::size_t N>
	bool checkMatchAndGetResult(Goal::Slice &target_slice,
	                            Goal::Slice &source_slice,
	                            std::array<int, N> &assignment) {
		HashType hash = hashSlicePair(target_slice, source_slice);
		return cachedData_[hash].checkMatchAndGetResult<N>(
		    target_slice, source_slice, assignment);
	}

	template <std::size_t N>
	void store(Goal::Slice &target_slice, Goal::Slice &source_slice,
	           std::array<int, N> &assignment) {
		HashType hash = hashSlicePair(target_slice, source_slice);
		cachedData_[hash].store<N>(target_slice, source_slice, assignment);
	}

protected:
	static constexpr HashType kBasePart_ = 50000017;
	static constexpr HashType kModPart_ = 100000007;
	static constexpr HashType kBaseSlice_ = 500000009;
	static constexpr HashType kModSlice_ = 1000000007;
	static constexpr HashType kInitialStartingHash_ = 1;
	EntriesContainer cachedData_;
};

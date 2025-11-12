/*
   Copyright 2025 Leil Storage OÜ

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

#include <algorithm>
#include <array>
#include <cstdint>
#include <random>
#include <vector>

/**
 * @class SkipList
 * @brief A probabilistic skip list implementation that mimics
 __gnu_pbds::tree_order_statistics_node_update
 *
 * @tparam Key The key type stored in the skip list
 * @tparam Cmp_Fn Comparator struct that defines a strict weak ordering (default: std::less<Key>)
 * @tparam prob_num Numerator of non-promotion probability P (default: 1)
 * @tparam prob_den Denominator of non-promotion probability P (default: 2)
 *
 * @details
 * This skip list implementation provides average O(log_{1/P} n) time complexity for search, insert,
 and delete operations.
 * The probability P = prob_num/prob_den determines the non-promotion probability during insertion.
 * number of levels across all n nodes = n/P, with expected complexity O(log_{1/P} n) for most
 operations.
 *
 * @warning Modifying the structure while iterating causes undefined behavior
 * @warning Caller is responsible for safe use of iterators, bad use will
 * result in undefined behavior.
 * @warning Don't modify anything in the key related to Compare function(eg. pointer references)
 * @warning THIS STRUCTURE IS NOT THREAD-SAFE.
 * @warning Memory is not released when a single SkipList is destroyed; it's only returned to the
 pool.
 * The memory for the pool itself is only released at program exit.

 * @note The implementation includes order statistics functionality (find_by_order, order_of_key)
 * @see https://en.wikipedia.org/wiki/Skip_list
 */
template <typename Key, typename Cmp_Fn = std::less<Key>, uint32_t prob_num = 1,
          uint32_t prob_den = 2>
class SkipList {
public:
	/// Type aliases for STL compatibility
	using key_type = Key;
	using key_compare = Cmp_Fn;
	using reference = key_type &;
	using const_reference = const key_type &;
	using size_type = uint64_t;
	using difference_type = std::ptrdiff_t;  ///< Difference type for iterators

	/// Constants for skip list configuration
	static constexpr int32_t kNullLevel = -1;  ///< Sentinel value for invalid level
	static constexpr size_type PointerSize = sizeof(uint64_t);
	static constexpr size_type kAlignOfPointer = sizeof(uint64_t);
	static constexpr size_type kMaxAllowedLevel = 128;  ///< Maximum allowed skip list height
	static constexpr size_type kAlignOfNode =
	    std::max({kAlignOfPointer, alignof(size_type), alignof(Key)});
	static constexpr size_type KeySize =  ///< Padded key size for alignment
	    sizeof(Key) + ((kAlignOfNode - sizeof(Key) % kAlignOfNode) % kAlignOfNode);

	/**
	 * @class SkipNodePool
	 * @brief Memory pool for efficient node allocation in skip list
	 *
	 * Manages memory allocation for skip nodes using a bucket-based approach
	 * to reduce memory fragmentation and improve cache performance.
	 * @details Every address in the pool is encoded in a 64-bit number. The first kLevelBits
	 * represent the level of the node, the next kOffsetBits store the index of the bucket
	 * in the pool, and the last kBucketBits store the index of the node in the bucket.
	 *
	 * The size of the bucket to allocate decrease along the structure size increases, this is an
	 * heuristic aimed to target the fragmentation that previous buckets may cause.
	 */
	class SkipNodePool {
	public:
		/// Memory pool configuration constants
		static constexpr uint64_t kLevelBits = 7;  ///< Bits reserved for level encoding
		static constexpr uint64_t kMaxLevel = (1 << kLevelBits);     ///< Maximum levels supported
		static constexpr uint64_t kBucketBits = 16;                  ///< Bits for ID encoding
		static constexpr uint64_t kBucketSize = (1 << kBucketBits);  ///< Nodes per bucket
		/// Bits for offset(bucket ID)
		static constexpr uint64_t kOffsetBits = 64 - kLevelBits - kBucketBits;
		static constexpr uint64_t kOffsetSize = (uint64_t(1) << kOffsetBits);  ///< Maximum offset
		static constexpr double kFragmentationFactor = 0.999;   ///< Fragmentation factor
		static constexpr double kMinFragmentationIndex = 0.03;  ///< Minimum fragmentation threshold

		/**
		 * @brief Constructs a new SkipNodePool
		 * @details Initializes free lists and memory pool arrays
		 */
		SkipNodePool() : fragmentationIndex_(1) {
			freeList_.assign(kMaxLevel, 0);
			pool_.assign(kMaxLevel, std::vector<std::byte *>());
		}

		/**
		 * @brief Destroys the SkipNodePool and deallocates all memory
		 */
		~SkipNodePool() {
			for (const auto &levelPool : pool_) {
				for (auto *bucket : levelPool) { delete[] bucket; }
			}
		}

		/**
		 * @brief Allocates a node with specified level
		 * @param level The skip list level for the node (0-based)
		 * @return uint64_t Encoded pointer to allocated node
		 * @throws std::bad_alloc if memory allocation fails
		 */
		uint64_t alloc(int level) {
			if (freeList_[level] == 0) { alloc_new_bucket_(level); }
			uint64_t pointer = freeList_[level];
			freeList_[level] = *static_cast<uint64_t *>(get_address(pointer));
			return pointer;
		}

		/**
		 * @brief Deallocates a node
		 * @param ptr Encoded pointer to the node to deallocate
		 */
		void dealloc(uint64_t ptr) {
			*static_cast<uint64_t *>(get_address(ptr)) = freeList_[ptr & (kMaxLevel - 1)];
			freeList_[ptr & (kMaxLevel - 1)] = ptr;
		}

		/**
		 * @brief Converts encoded pointer to actual memory address
		 * @param ptr Encoded pointer to convert
		 * @return void* Actual memory address of the node
		 */
		void *get_address(uint64_t ptr) const {
			uint64_t level = ptr & (kMaxLevel - 1);
			uint64_t bucketId = (ptr >> kLevelBits) & (kOffsetSize - 1);
			uint64_t idx = (ptr >> (kLevelBits + kOffsetBits)) & (kBucketSize - 1);
			return pool_[level][bucketId] +
			       (idx * (KeySize + PointerSize + level * (PointerSize + sizeof(size_type))));
		}

	private:
		/**
		 * @brief Allocates a new bucket of nodes for a specific level
		 * @param level The level to allocate bucket for
		 */
		void alloc_new_bucket_(uint64_t level) {
			/// As memory fragments, the size of the chunks gradually decreases.
			uint32_t levelBucketSize = kBucketSize * fragmentationIndex_ / (level + 1);
			fragmentationIndex_ =
			    std::max(fragmentationIndex_ * kFragmentationFactor, kMinFragmentationIndex);

			pool_[level].emplace_back(
			    new std::byte[levelBucketSize *
			                  (KeySize + PointerSize + level * (PointerSize + sizeof(size_type)))]);
			/**
			 * For level 0, the very first bucket (i.e., when pool_[0].size() == 1) must reserve
			 * address 0 to represent nullptr in the skip list. This means the node at offset 0
			 * in this bucket cannot be used for actual data, so we skip i = 0 in this case.
			 * For all other levels or subsequent buckets, all offsets are valid.
			 */
			for (int i = levelBucketSize - 1; i >= (level == 0 && pool_[level].size() == 1); i--) {
				dealloc(level | ((pool_[level].size() - 1) << kLevelBits) |
				        (uint64_t(i) << (kLevelBits + kOffsetBits)));
			}
		}

		double fragmentationIndex_{1.0};
		std::vector<uint64_t> freeList_;
		std::vector<std::vector<std::byte *>> pool_;
	};

	// Static members for memory management and randomization
	static inline SkipNodePool nodePool;
	static inline std::uniform_real_distribution<double> randDistribution{0.0, 1.0};

#if !defined(NDEBUG)
	static inline std::mt19937_64 generator;
#else
	static inline std::mt19937_64 generator{std::random_device{}()};
#endif

	/**
	 * @class pointer
	 * @brief Smart pointer-like class for skip list nodes
	 *
	 * Provides type-safe access to node members while using encoded addresses
	 * for memory efficiency.
	 * @details The actual Node class is not directly implemented. Instead,
	 * node data is accessed through this Pointer class which decodes
	 * memory addresses and provides access to node members via member functions key(), next0(),
	 * and next().
	 *
	 * The span of level 0 is always 1. For memory optimization reasons, this variable was
	 * removed and the 'next' array was split into 'next0' for the level 0 pointer to the
	 * next node, and 'next' for pointers and spans of the other levels starting from level 1.
	 */
	class pointer {
	public:
		/// @brief Default constructor (null pointer)
		pointer() : address_(0) {}

		/// @brief Construct from nullptr
		pointer(std::nullptr_t) : address_(0) {}

		/**
		 * @brief Explicit constructor from encoded address
		 * @param _address Encoded node address
		 */
		explicit pointer(uint64_t _address) : address_(_address) {}

		/**
		 * @brief Assignment from nullptr
		 * @return pointer& Reference to this pointer
		 */
		pointer &operator=(std::nullptr_t) {
			address_ = 0;
			return *this;
		}

		/// Comparison operators
		bool operator==(const pointer &other) const { return address_ == other.address_; }
		bool operator!=(const pointer &other) const { return !(*this == other); }
		bool operator==(std::nullptr_t) const { return address_ == 0; }
		bool operator!=(std::nullptr_t) const { return address_ != 0; }
		friend bool operator==(std::nullptr_t, const pointer &ptr) { return ptr.address_ == 0; }
		friend bool operator!=(std::nullptr_t, const pointer &ptr) { return ptr.address_ != 0; }

		/// Cast operators
		explicit operator bool() const { return address_ != 0; }
		operator uint64_t() const { return address_; }

		/// Access methods read directly from raw buffer

		/**
		 * @brief Key member of node
		 * @return key_type& Reference to the key of the node
		 */
		key_type &key() {
			void *raw = nodePool.get_address(address_);
			return *std::launder(reinterpret_cast<key_type *>(raw));
		}

		/**
		 * @brief Key member of node
		 * @return const key_type& Constant reference to the key of the node
		 */
		const key_type &key() const {
			void *raw = nodePool.get_address(address_);
			return *reinterpret_cast<const key_type *>(raw);
		}

		/**
		 * @brief  next0 member of node
		 * @return pointer& Reference to the pointer in level 0 of array "next"
		 */
		pointer &next0() {
			void *raw = nodePool.get_address(address_);
			auto *byte_ptr = static_cast<std::byte *>(raw);

			size_t offset = KeySize;
			return *std::launder(reinterpret_cast<pointer *>(byte_ptr + offset));
		}

		/**
		 * @brief  next0 member of node
		 * @return const pointer& Constant reference to the pointer in level 0 of array "next"
		 */
		const pointer &next0() const {
			void *raw = nodePool.get_address(address_);
			auto *byte_ptr = static_cast<std::byte *>(raw);

			size_t offset = KeySize;
			return *reinterpret_cast<const pointer *>(byte_ptr + offset);
		}

		/**
		 * @brief Gets the forward pointers array for higher levels
		 * @return std::pair<pointer, size_type>* Array of (pointer, count) pairs,
		 * is 1-indexed for memory reduction, count of level 0 is always 1.
		 */
		std::pair<pointer, size_type> *next() {
			void *raw = nodePool.get_address(address_);
			std::byte *array_start = static_cast<std::byte *>(raw) + KeySize + PointerSize;
			return std::launder(reinterpret_cast<std::pair<pointer, size_type> *>(array_start));
		}

		/**
		 * @brief Gets the forward pointers array for higher levels
		 * @return const std::pair<pointer, size_type>* Array of (pointer, count) pairs,
		 * is 1-indexed for memory reduction, count of level 0 is always 1.
		 */
		const std::pair<pointer, size_type> *next() const {
			void *raw = nodePool.get_address(address_);
			std::byte *array_start = static_cast<std::byte *>(raw) + KeySize + PointerSize;
			return reinterpret_cast<const std::pair<pointer, size_type> *>(array_start);
		}

		/**
		 * @brief Creates a new node with specified key and level
		 * @param key The key to store in the node
		 * @param level The skip list level for this node
		 * @return pointer Pointer to the created node
		 */
		static pointer create(const key_type &key, size_type level) {
			pointer ptr(nodePool.alloc(level));
			new (&ptr.key()) key_type(key);
			ptr.next0() = nullptr;
			// Initialize next array
			for (size_type i = 0; i < level; i++) { ptr.next()[i] = {nullptr, 0}; }
			return ptr;
		}

		/**
		 * @brief Creates a copy of a node
		 * @param ptToCopy The pointer of the node to copy
		 * @return pointer Pointer to the created node
		 */
		static pointer create(const pointer &ptToCopy) {
			pointer ptr(nodePool.alloc(ptToCopy.get_level()));
			new (&ptr.key()) key_type(ptToCopy.key());
			ptr.next0() = ptToCopy.next0();
			// Initialize next array
			for (size_type i = 0; i < ptr.get_level(); i++) { ptr.next()[i] = ptToCopy.next()[i]; }
			return ptr;
		}

		/**
		 * @brief Destroys a node and returns it to the pool
		 * @param pt Pointer to the node to destroy
		 */
		static void destroy(pointer ptr) {
			if (ptr != nullptr) {
				ptr.key().~key_type();
				nodePool.dealloc(ptr);
			}
		}

	private:
		/// @brief Calculate the level of the node from address_
		/// @return uint64_t The level of the node
		uint64_t get_level() const { return address_ & (SkipNodePool::kMaxLevel - 1); }

		uint64_t address_;  ///< Encoded node address_
	};

	/// Static array for update operations to avoid dynamic allocation
	static inline std::array<std::pair<pointer, size_type>, kMaxAllowedLevel> update;

	/**
	 * @class const_iterator
	 * @brief Forward const_iterator for skip list traversal
	 *
	 * Provides STL-compatible iterator interface for traversing
	 * the skip list in sorted order.
	 */
	class const_iterator {
	public:
		// STL iterator traits
		using iterator_category = std::forward_iterator_tag;
		using value_type = const Key;
		using difference_type = std::ptrdiff_t;
		using reference = const value_type &;

		/**
		 * @brief Constructs an iterator
		 * @param _ptr Pointer to the current node
		 * @param _idx Index of the current node
		 */
		const_iterator(pointer _ptr = nullptr, size_type _idx = 0) : nodePtr_(_ptr), idx_(_idx) {}

		/// Dereference and member access operators
		const key_type &operator*() const { return nodePtr_.key(); }
		const key_type *operator->() const { return &**this; }

		/// Increment operators
		const_iterator &operator++() {
			++idx_;
			nodePtr_ = nodePtr_.next0();
			return *this;
		}
		const_iterator operator++(int) {
			const_iterator tmp = *this;
			++(*this);
			return tmp;
		}

		/// Comparison operators
		bool operator==(const const_iterator &other) const {
			return nodePtr_ == other.nodePtr_ && idx_ == other.idx_;
		}
		bool operator!=(const const_iterator &other) const { return !(*this == other); }

		/// Gets the current index of the const_iterator
		size_type get_index() const { return idx_; }

	private:
		pointer nodePtr_;  ///< Current node pointer
		size_type idx_;    ///< Current index in sorted order
	};

	using value_type = Key;
	using pointer_type = pointer;
	using iterator = const_iterator;  ///< Const iterator type (same as iterator)

	/**
	 * @brief Returns an iterator to the first element
	 * @return iterator Iterator to the first element
	 */
	const_iterator begin() const { return const_iterator(this->head_.next0(), 0); }

	/**
	 * @brief Returns an iterator to the element following the last element
	 * @return iterator Iterator to the end
	 */
	const_iterator end() const { return const_iterator(nullptr, size_); }

	/**
	 * @brief Default constructor
	 * @details Creates an empty skip list with a head node
	 */
	SkipList() : head_(pointer::create(key_type(), 0)) { head_.next0() = nullptr; }

	/**
	 * @brief Destructor
	 * @details Destroys all nodes and deallocates memory
	 */
	~SkipList() {
		pointer temp = head_;
		pointer old = head_;
		while (temp != nullptr) {
			old = temp;
			temp = old.next0();
			pointer::destroy(old);
		}
	}

	/**
	 * @brief Copy constructor (Deep Copy).
	 * @details Create a new SkipList copying all the element from 'other'.
	 * Complexity: O(N * logN)
	 * @param other SkipList from which to copy.
	 */
	SkipList(const SkipList &other)
	    : head_(pointer::create(other.head_)), size_(other.size_), lastLevel_(other.lastLevel_) {
		for (uint64_t i = 0; i <= lastLevel_; i++) { update[i] = {nullptr, 0}; }

		// Make a perfect copy of the list.
		for (auto [it, idx] = std::pair{other.head_, size_type(0)}; it != nullptr;
		     it = it.next0(), idx++) {
			pointer current;
			// Copy the span array of each node exactly as it is, the pointers will be substituted
			// later
			if (idx > 0) {
				current = pointer::create(it);
			} else {
				// The head is already created
				current = head_;
			}
			for (uint64_t i = 1; i <= lastLevel_; i++) {
				// update[i].first keeps the pointer of the last active node of this level
				// update[i].second keeps the index of the next active node of this level
				// when update[i].second == idx it means the current node is the node next to
				// update[i].first in this level.
				if (update[i].second == idx) {
					if (update[i].first != nullptr) {
						update[i].first.next()[i - 1].first = current;
					}
					update[i].first = current;
					update[i].second = current.next()[i - 1].second + idx;
				}
			}
			if (update[0].first != nullptr) { update[0].first.next0() = current; }
			update[0].first = current;
		}
		/// For those pointers which have the .end() as the next pointer in their current level
		for (uint64_t i = 1; i <= lastLevel_; i++) {
			update[i].first.next()[i - 1].first = nullptr;
		}
		update[0].first.next0() = nullptr;
	}

	/**
	 * @brief Movement constructor.
	 * @details "Steal" all the nodes and the state from 'other' in O(1).
	 * 'other' stays in an empty but valid state.
	 * Complexity: O(1)
	 * @param other The SkipList from which to move (it will be emptied)
	 */
	SkipList(SkipList &&other) noexcept : SkipList() { other.swap(*this); }

	/**
	 * @brief Copy assignment operator.
	 * @details Uses the copy-and-swap idiom.
	 * @param other SkipList from which to copy.
	 * Complexity: O(N * logN)
	 * @return Reference to *this.
	 */
	SkipList &operator=(const SkipList &other) {
		// Handle self-assignment (eg. list1 = list1;)
		if (this == &other) { return *this; }
		SkipList temp(other);
		swap(temp);

		/// temp is freed automatically
		return *this;
	}

	/**
	 * @brief Movement assignment operator
	 * @details Free current resources and "steal" from 'other'.
	 * @param other The SkipList from which to move (it will be emptied).
	 * Complexity: O(N)
	 * @return Reference *this.
	 */
	SkipList &operator=(SkipList &&other) noexcept {
		// Handle self-assignment (eg. list1 = std::move(list1);)
		if (this == &other) { return *this; }

		clear();
		swap(other);

		return *this;
	}

	/**
	 * @brief Swap the contents of this list and 'other'.
	 * @param other SkipList to swap.
	 */
	void swap(SkipList &other) noexcept {
		using std::swap;

		swap(head_, other.head_);
		swap(size_, other.size_);
		swap(lastLevel_, other.lastLevel_);
	}

	/**
	 * @brief Finds the first element not less than the given key
	 * @param elem The key to search for
	 * @return const_iterator Iterator to the first element k where key_compare(k,elem)
	 * returns false or end() if no element fulfilling the condition exists
	 *
	 * Complexity O(log n)
	 */
	const_iterator lower_bound(const key_type &elem) const {
		pointer current = head_;
		size_type sum = 0;

		for (int level = lastLevel_; level > 0; level--) {
			while (current.next()[level - 1].first != nullptr &&
			       !key_compare()(elem, current.next()[level - 1].first.key())) {
				sum += current.next()[level - 1].second;
				current = current.next()[level - 1].first;
			}
		}
		while (current.next0() != nullptr && !key_compare()(elem, current.next0().key())) {
			sum += 1;
			current = current.next0();
		}

		if (current == head_) { return this->begin(); }

		if (!key_compare()(elem, current.key()) && !key_compare()(current.key(), elem)) {
			return const_iterator(current, sum - 1);
		}
		sum += 1;
		current = current.next0();
		if (current == nullptr) { return this->end(); }
		return const_iterator(current, sum - 1);
	}

	/**
	 * @brief Finds an element with the given key
	 * @param elem The key to search for
	 * @return const_iterator Iterator to the element, or end() if not found
	 *
	 * Complexity O(log n)
	 */
	const_iterator find(const key_type &elem) const {
		const_iterator current = lower_bound(elem);
		if (current != this->end() && !key_compare()(elem, *current) &&
		    !key_compare()(*current, elem)) {
			return current;
		}
		return this->end();
	}

	/**
	 * @brief Inserts an element into the skip list
	 * @param elem The key to insert
	 * @return std::pair<const_iterator, bool> Iterator to inserted element and bool indicating
	 * success
	 *
	 * Complexity O(log n)
	 */
	std::pair<const_iterator, bool> insert(const key_type &elem) {
		pointer current = head_;
		size_type sum = 0;

		for (int level = lastLevel_; level > 0; level--) {
			while (current.next()[level - 1].first != nullptr &&
			       !key_compare()(elem, current.next()[level - 1].first.key())) {
				sum += current.next()[level - 1].second;
				current = current.next()[level - 1].first;
			}
			update[level] = {current, sum};
		}
		while (current.next0() != nullptr && !key_compare()(elem, current.next0().key())) {
			sum += 1;
			current = current.next0();
		}
		update[0] = {current, sum};

		if (sum > 0 && !key_compare()(elem, current.key()) && !key_compare()(current.key(), elem)) {
			return {iterator(current, sum - 1), false};
		}

		uint32_t newLevel = 0;
		while (randDistribution(generator) * prob_den > prob_num && newLevel <= lastLevel_) {
			newLevel++;
		}
		if (newLevel == kMaxAllowedLevel) { newLevel--; }
		if (newLevel > lastLevel_) {
			pointer newHead = pointer::create(head_.key(), newLevel);
			for (uint32_t level = 0; level < newLevel; level++) {
				if (level > 0) {
					newHead.next()[level - 1] = head_.next()[level - 1];
				} else {
					newHead.next0() = head_.next0();
				}
				if (update[level].first == head_) { update[level].first = newHead; }
			}
			update[newLevel].first = newHead;
			update[newLevel].second = 0;
			newHead.next()[newLevel - 1] = {nullptr, size_ + 1};
			pointer::destroy(head_);
			head_ = newHead;
			lastLevel_ = newLevel;
		}

		pointer newNode = pointer::create(elem, newLevel);
		newNode.next0() = update[0].first.next0();
		update[0].first.next0() = newNode;
		// Update array next of involved nodes
		for (uint32_t level = 1; level <= lastLevel_; level++) {
			if (level <= newLevel) {
				newNode.next()[level - 1].first = update[level].first.next()[level - 1].first;
				newNode.next()[level - 1].second =
				    update[level].second + update[level].first.next()[level - 1].second - sum;
				update[level].first.next()[level - 1].first = newNode;
				update[level].first.next()[level - 1].second = sum + 1 - update[level].second;
			} else {
				update[level].first.next()[level - 1].second++;
			}
		}
		size_++;
		return {const_iterator(newNode, sum), true};
	}

	/**
	 * @brief Removes an element from the skip list
	 * @param elem The key to remove
	 * @return size_type Number of elements removed (0 or 1)
	 *
	 * Complexity O(log n)
	 */
	size_type erase(const key_type &elem) {
		pointer current = head_;
		size_type sum = 0;
		int32_t delLevel = kNullLevel;

		for (int level = lastLevel_; level > 0; level--) {
			while (current.next()[level - 1].first != nullptr &&
			       key_compare()(current.next()[level - 1].first.key(), elem)) {
				sum += current.next()[level - 1].second;
				current = current.next()[level - 1].first;
			}
			if (delLevel == kNullLevel) {
				if (current.next()[level - 1].first != nullptr &&
				    !key_compare()(elem, current.next()[level - 1].first.key())) {
					delLevel = level;
				}
			}
			update[level] = {current, sum};
		}
		while (current.next0() != nullptr && key_compare()(current.next0().key(), elem)) {
			sum += 1;
			current = current.next0();
		}
		if (delLevel == kNullLevel) {
			if (current.next0() != nullptr && !key_compare()(elem, current.next0().key())) {
				delLevel = 0;
			}
		}
		update[0] = {current, sum};

		if (delLevel == kNullLevel) { return 0; }
		pointer target = current.next0();
		update[0].first.next0() = target.next0();
		// Update array next of involved nodes
		for (uint32_t level = 1; level <= lastLevel_; level++) {
			if (int(level) <= delLevel) {
				update[level].first.next()[level - 1].second += target.next()[level - 1].second - 1;
				update[level].first.next()[level - 1].first = target.next()[level - 1].first;
			} else {
				update[level].first.next()[level - 1].second--;
			}
		}
		pointer::destroy(target);
		size_--;
		return 1;
	}

	/**
	 * @brief Removes the element at the given iterator position
	 * @param itr Iterator to the element to remove
	 * @return size_type Number of elements removed (0 or 1)
	 *
	 * @warning The iterator becomes invalid after erasure
	 * Complexity O(log n)
	 */
	size_type erase(const const_iterator &itr) {
		pointer current = head_;
		size_type sum = 0;
		int32_t delLevel = kNullLevel;

		for (int level = lastLevel_; level > 0; level--) {
			while (current.next()[level - 1].first != nullptr &&
			       sum + current.next()[level - 1].second - 1 < itr.get_index()) {
				sum += current.next()[level - 1].second;
				current = current.next()[level - 1].first;
			}
			if (delLevel == kNullLevel) {
				if (current.next()[level - 1].first != nullptr &&
				    sum + current.next()[level - 1].second - 1 == itr.get_index()) {
					delLevel = level;
				}
			}
			update[level] = {current, sum};
		}
		while (current.next0() != nullptr && sum < itr.get_index()) {
			sum += 1;
			current = current.next0();
		}
		if (delLevel == kNullLevel) {
			if (current.next0() != nullptr && sum == itr.get_index()) { delLevel = 0; }
		}
		update[0] = {current, sum};

		if (delLevel == kNullLevel) { return 0; }
		pointer target = current.next0();
		update[0].first.next0() = target.next0();
		// Update array next of involved nodes
		for (uint32_t level = 1; level <= lastLevel_; level++) {
			if (int(level) <= delLevel) {
				update[level].first.next()[level - 1].second += target.next()[level - 1].second - 1;
				update[level].first.next()[level - 1].first = target.next()[level - 1].first;
			} else {
				update[level].first.next()[level - 1].second--;
			}
		}
		pointer::destroy(target);
		size_--;
		return 1;
	}

	/**
	 * @brief Finds the element at the given position in sorted order
	 * @param nth The zero-based index to find
	 * @return const_iterator Iterator to the nth element in sorted order
	 *
	 * Complexity O(log n)
	 */
	const_iterator find_by_order(size_type nth) const {
		size_type sum = 0;
		pointer current = head_;

		for (int level = lastLevel_; level > 0; level--) {
			if (current == nullptr) { break; }
			while (current != nullptr && sum + current.next()[level - 1].second <= nth + 1) {
				sum += current.next()[level - 1].second;
				current = current.next()[level - 1].first;
			}
		}
		while (current != nullptr && sum <= nth) {
			sum += 1;
			current = current.next0();
		}
		return const_iterator(current, sum - 1);
	}

	/**
	 * @brief Finds the position of the given key in sorted order
	 * @param elem The key to find the position of
	 * @return size_type The zero-based index of the key, or size() if not found
	 *
	 * Complexity O(log n)
	 */
	size_type order_of_key(const key_type &elem) const {
		auto itr = lower_bound(elem);
		return itr.get_index();
	}

	/**
	 * @brief Removes all elements from the skip list
	 * @post size() == 0
	 */
	void clear() {
		pointer temp = head_;
		pointer old = head_;
		while (temp != nullptr) {
			old = temp;
			temp = old.next0();
			pointer::destroy(old);
		}
		lastLevel_ = 0;
		size_ = 0;
		head_ = pointer::create(key_type(), 0);
		head_.next0() = nullptr;
	}

	/**
	 * @brief Returns the number of elements in the skip list
	 * @return size_type Number of elements
	 */
	size_type size() const { return size_; }

	/**
	 * @brief Checks if the skip list is empty
	 * @return true if skip list is empty
	 * @return false otherwise
	 */
	bool empty() const { return size_ == 0; }

private:
	pointer head_;         ///< Head node of the skip list
	size_type size_{};     ///< Number of elements in the skip list
	uint8_t lastLevel_{};  ///< Current maximum level in the skip list
};

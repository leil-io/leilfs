/*

   Copyright 2017 Skytechnology sp. z o.o.
   Copyright 2023 Leil Storage OÜ


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

#pragma once

#include "common/platform.h"

#include <unordered_map>

#include "common/observable_property.h"
#include "common/richacl.h"
#include "common/type_defs.h"

/// Signal emitted whenever an inode's ACL changes (set, mode-synced, or removed).
/// Parameter: inode id. Handlers should read the current ACL from gMetadata->aclStorage to persist
/// (ACLS_<inode>) or remove it. ACLs are not part of the metadata checksum; they are rebuilt from
/// node loading order via this durable section on the forkless backend.
inline Signal<inode_t> gAclChangedSignal;

/*!
 * \brief A class aggregating ACL storage and inode->acl maps, deduplication included.
 */
class AclStorage {
public:
	AclStorage(const AclStorage&) = delete;
	AclStorage& operator=(const AclStorage&) = delete;
	AclStorage(AclStorage&&) = delete;
	AclStorage& operator=(AclStorage&&) = delete;

	AclStorage() = default;

	/*!
	 * \brief Assert state sanity.
	 */
	~AclStorage();

	/*!
	 * \brief Find ACL for an inode.
	 *
	 * \param id inode id
	 * \return ACL mapped to id or nullptr if none
	 */
	const RichACL *get(inode_t id) const;

	/*!
	 * \brief Set ACL for an inode.
	 *
	 * \param id inode id
	 * \param acl ACL to be set
	 */
	void set(inode_t id, RichACL &&acl);

	/*!
	 * \brief Erase ACL for an inode.
	 *
	 * \param id inode id
	 */
	void erase(inode_t id);

	/*!
	 * \brief Set mode of ACL of an inode (if any).
	 *
	 * \param id inode id
	 * \param mode mode to be set
	 * \param is_dir true if inode is a directory
	 */
	void setMode(inode_t id, uint16_t mode, bool is_dir);
private:
	struct Hash {
		size_t operator()(const RichACL &acl) const;
	};

	using AclToRefCountMap = std::unordered_map<RichACL, unsigned long, Hash>;
	using KeyValue = AclToRefCountMap::value_type;
	using InodeToKVMap = std::unordered_map<inode_t, std::reference_wrapper<KeyValue>>;

	/*!
	 * \brief Insert an ACL into permanent storage.
	 *
	 * Insert a key-value (ACL, reference count) into storage or increase
	 * reference count of one already present.
	 *
	 * \param acl key
	 * \return key-value pair from storage
	 */
	KeyValue &ref(RichACL &&acl);

	/*!
	 * \brief Decrease reference count of an ACL.
	 *
	 * Decrease reference count of a key-value pair from storage and remove
	 * it from storage if count reaches zero.
	 *
	 * \param kv key-value pair from storage
	 */
	void unref(KeyValue &kv);

	AclToRefCountMap storage_;
	InodeToKVMap acl_;
};

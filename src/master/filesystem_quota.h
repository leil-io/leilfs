/*
   2013-2015 Skytechnology sp. z o.o.

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

#pragma once

#include "common/platform.h"

#include <cstdint>
#include <initializer_list>
#include <utility>

#include "common/quota_database.h"
#include "master/filesystem_freenode.h"
#include "master/filesystem_node_types.h"
#include "protocol/quota.h"

class FsContext;

/*! \brief Test if resource change exceeds quota for users and groups.
 * \param uid User id.
 * \param gid Group id.
 * \param resource_list Required changes to resources.
 * \return true if quota is exceeded.
 */
bool fsnodes_quota_exceeded_ug(uint32_t uid, uint32_t gid,
	const std::initializer_list<std::pair<QuotaResource, int64_t>> &resource_list);

/*! \brief Test if resource change exceeds quota for users and groups.
 * \param node Pointer to node with user id and group id that is used to check quota.
 * \param resource_list Required changes to resources.
 * \return true if quota is exceeded.
 */
bool fsnodes_quota_exceeded_ug(FSNode *node,
	const std::initializer_list<std::pair<QuotaResource, int64_t>> &resource_list);

/*! \brief Test if resource change exceeds quota for directories.
 * \param node Pointer to node in directory tree to check quota for.
 * \param resource_list Required changes to resources.
 * \return true if quota is exceeded.
 */
bool fsnodes_quota_exceeded_dir(FSNode *node,
	const std::initializer_list<std::pair<QuotaResource, int64_t>> &resource_list);

/*! \brief Test if moving node (moving resources from one parent to other) exceeds quota.
 * \param node Destination parent.
 * \param prev_node Source parent.
 * \param resource_list required changes to quota.
 * \return true if quota is exceeded.
 */
bool fsnodes_quota_exceeded_dir(FSNodeDirectory *node, FSNodeDirectory* prev_node,
	const std::initializer_list<std::pair<QuotaResource, int64_t>> &resource_list);


/*! \brief Test if resource change exceeds quota for user+groups and directories.
 * \param node Pointer to node in directory tree to check quota for. User id and group id is taken
 *             from node.
 * \param resource_list Required changes to resources.
 * \return true if quota is exceeded.
 */
bool fsnodes_quota_exceeded(FSNode *node,
	const std::initializer_list<std::pair<QuotaResource, int64_t>> &resource_list);

/*! \brief Update quota for both user+group and directory.
 * \param node Pointer to node in directory tree to update quota for. User id and group id is taken
 *             from node.
 * \param resource_list Required changes to quota.
 * \return true if quota is exceeded.
 */
void fsnodes_quota_update(FSNode *node,
	const std::initializer_list<std::pair<QuotaResource, int64_t>> &resource_list);

/*! \brief Remove quota.
 * \param owner_type Owner type (user, group, inode(directory)).
 * \param owner_id Owner id.
 */
void fsnodes_quota_remove(QuotaOwnerType owner_type, uint32_t owner_id);

/*! \brief Adjust reported free/total space based on quota information.
 * \param node Pointer to root node in directory tree that we should adjust space for.
 * \param total_space Totals space (used + free).
 * \param available_space Free space.
 */
void fsnodes_quota_adjust_space(FSNode *node, uint64_t &total_space, uint64_t &available_space);

namespace quotas {
uint8_t fs_quota_get_all(const FsContext &context, std::vector<QuotaEntry> &results);
uint8_t fs_quota_get(const FsContext &context, const std::vector<QuotaOwner> &owners,
                     std::vector<QuotaEntry> &results);
uint8_t fs_quota_set(const FsContext &context, const std::vector<QuotaEntry> &entries);
uint8_t fs_quota_get_info(const FsContext &context, const std::vector<QuotaEntry> &entries,
                          std::vector<std::string> &result);

uint8_t fs_apply_setquota(char rigor, char resource, char ownerType, inode_t ownerId,
                          uint64_t limit);
}  // namespace quotas

/**
 * @file   saunafs_fsal_types.h
 * @author Crash <crash@leil.io>
 *
 * @brief File System Abstraction Layer types and constants.
 *
 * This file includes declarations of data types, variables and constants
 * for SaunaFS FSAL.
 */

#pragma once

#include "fsal_api.h"

#include "fileinfo_cache.h"
#include "mount/client/saunafs_c_api.h"

#define SAUNAFS_VERSION(major, minor, micro) \
                (0x010000 * major + 0x0100 * minor + micro)
#define kDisconnectedChunkServerVersion SAUNAFS_VERSION(256, 0, 0)

#define SFS_NAME_MAX 255

static const int kNFS4_ERROR = -1;

#ifndef SFSBLOCKSIZE
#define SFSBLOCKSIZE 65536
#endif

#define SPECIAL_INODE_BASE 0xFFFFFFF0U
#define SPECIAL_INODE_ROOT 0x01U
#define MAX_REGULAR_INODE  (SPECIAL_INODE_BASE - 0x01U)

#define SAUNAFS_SUPPORTED_ATTRS                                           \
	(ATTR_TYPE | ATTR_SIZE | ATTR_FSID | ATTR_FILEID | ATTR_MODE |        \
	 ATTR_NUMLINKS | ATTR_OWNER | ATTR_GROUP | ATTR_ATIME | ATTR_CTIME |  \
	 ATTR_MTIME | ATTR_CHANGE | ATTR_SPACEUSED | ATTR_RAWDEV | ATTR_ACL | \
	 ATTR4_XATTR)

#define SAUNAFS_BIGGEST_STRIPE_COUNT 4096
#define SAUNAFS_STD_CHUNK_PART_TYPE  0
#define SAUNAFS_EXPECTED_BACKUP_DS_COUNT 3
#define TCP_PROTO_NUMBER 6

typedef sau_fileinfo_t fileinfo_t;

/**
 * @struct SaunaFSModule saunafs_fsal_types.h [saunafs_fsal_types.h]
 *
 * @brief SaunaFS Main global module object.
 *
 * SaunaFSModule contains the global module object, FSAL object
 * operations vector and parameters of the filesystem info.
 */
struct SaunaFSModule {
	struct fsal_module fsal;
	struct fsal_obj_ops handleOperations;
	fsal_staticfsinfo_t filesystemInfo;
};

extern struct SaunaFSModule SaunaFS;

struct SaunaFSHandle;

/**
 * @struct SaunaFSExport saunafs_fsal_types.h [saunafs_fsal_types.h]
 *
 * @brief SaunaFS private export object.
 *
 * SaunaFSExport contains information related with the export,
 * the filesystem operations, the parameters used to connect
 * to the master server, the cache used and the pNFS support.
 */
struct SaunaFSExport {
	struct fsal_export export; /// Export object
	struct SaunaFSHandle *root; /// root handle of export

	sau_t *fsInstance; /// Filesystem instance
	sau_init_params_t parameters; /// Initial parameters
	FileInfoCache_t *cache; /// Export cache

	bool pnfsMdsEnabled; /// pNFS Metadata Server enabled
	bool pnfsDsEnabled;  /// pNFS Data Server enabled

	uint32_t cacheTimeout; /// Timeout for entries at cache
	uint32_t cacheMaximumSize; /// Maximum size of cache
};

/**
 * @struct SaunaFSFd saunafs_fsal_types.h [saunafs_fsal_types.h]
 *
 * @brief SaunaFS FSAL file descriptor.
 *
 * SaunaFSFd works as a container to manage the information of a SaunaFS
 * file descriptor and its flags associated like open and share mode.
 */
struct SaunaFSFd {
	fsal_openflags_t openflags; /// The open and share mode
	struct sau_fileinfo *fd; /// SaunaFS file descriptor
};

/**
 * @struct SaunaFSStateFd saunafs_fsal_types.h [saunafs_fsal_types.h]
 *
 * @brief Associates a single NFSv4 state structure with a file descriptor.
 */
struct SaunaFSStateFd {
	/// Structure representing a single NFSv4 state
	struct state_t state;
	/// SaunaFS file descriptor associated with the state
	struct SaunaFSFd saunafsFd;
};

struct SaunaFSHandleKey {
	uint16_t moduleId; /// module id
	uint16_t exportId; /// export id
	sau_inode_t inode; /// inode
};

/**
 * @struct SaunaFSHandle saunafs_fsal_types.h [saunafs_fsal_types.h]
 *
 * @brief SaunaFS FSAL handle.
 *
 * SaunaFSHandle contains information related with the public structure of the
 * filesystem and its operations.
 */
struct SaunaFSHandle {
	struct fsal_obj_handle handle; /// Public handle
	struct SaunaFSFd fd; /// SaunaFS FSAL file descriptor
	sau_inode_t inode; /// inode of file
	struct SaunaFSHandleKey key; /// Handle key
	struct SaunaFSExport *export; /// Export to which the handle belongs
	struct fsal_share share; /// The ref counted share reservation state
};

struct DSWire {
	inode_t inode; /// inode
};

struct DataServerHandle {
	struct fsal_ds_handle handle; /// Public Data Server handle
	inode_t inode; /// inode
	FileInfoEntry_t *cacheHandle; /// Cache entry for inode
};

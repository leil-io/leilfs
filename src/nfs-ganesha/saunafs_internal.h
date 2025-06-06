/**
 * @file   saunafs_internal.h
 * @author Crash <crash@leil.io>
 *
 * @brief Function definitions for SaunaFS FSAL
 */

#pragma once

#include "FSAL/fsal_localfs.h"
#include "saunafs_fsal_types.h"

sau_context_t *createContext(sau_t *instance, struct user_cred *cred);

void exportOperationsInit(struct export_ops *ops);
void handleOperationsInit(struct fsal_obj_ops *ops);

/// Functions for allocating/deleting handles
struct SaunaFSHandle *allocateHandle(const struct stat *attribute,
                                     struct SaunaFSExport *export);

void deleteHandle(struct SaunaFSHandle *object);

// Functions for ACL
fsal_status_t getACL(struct SaunaFSExport *export, inode_t inode,
                     uint32_t ownerId, fsal_acl_t **acl);

fsal_status_t setACL(struct SaunaFSExport *export, inode_t inode,
                     const fsal_acl_t *acl, unsigned int mode);

// Functions for handling errors
fsal_status_t saunafsToFsalError(sau_err_t errorCode);
fsal_status_t fsalLastError(void);
nfsstat4 nfs4LastError(void);

// Functions for pNFS
void pnfsMdsOperationsInit(struct fsal_ops *ops);
void exportOperationsPnfs(struct export_ops *ops);

void pnfsDsOperationsInit(struct fsal_pnfs_ds_ops *ops);
void handleOperationsPnfs(struct fsal_obj_ops *ops);

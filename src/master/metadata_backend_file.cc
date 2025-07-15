/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


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

#include "master/metadata_backend_file.h"

#include <fcntl.h>  // for open and O_RDONLY
#include <sys/mman.h>
#include <sys/stat.h>
#include <cstdint>
#include <memory>

#include <common/cwrap.h>
#include <common/event_loop.h>
#include <common/rotate_files.h>
#include <common/saunafs_version.h>
#include <common/setup.h>
#include <common/type_defs.h>
#include <master/changelog.h>
#include <master/chunks.h>
#include <master/filesystem.h>
#include <master/filesystem_metadata.h>
#include <master/filesystem_node.h>
#include <master/filesystem_node_types.h>
#include <master/filesystem_operations.h>
#include <master/filesystem_quota.h>
#include <master/filesystem_store_acl.h>
#include <master/matoclserv.h>
#include <master/matomlserv.h>
#include <master/metadata_backend_common.h>
#include <master/metadata_dumper_file.h>
#include <master/restore.h>
#include <slogger/slogger.h>
#include "protocol/SFSCommunication.h"

MetadataBackendFile::MetadataBackendFile()
#if !defined(METARESTORE) && !defined(METALOGGER)
    : dumper_(std::make_unique<MetadataDumperFile>(kMetadataFilename, kMetadataTmpFilename))
#endif  // #if !defined(METARESTORE) && !defined(METALOGGER)
{
	safs::log_info("Metadata backend: {}", backendType());
}

#if !defined(METARESTORE) && !defined(METALOGGER)

bool MetadataBackendFile::commit_metadata_dump() {
	rotateFiles(kMetadataFilename, gStoredPreviousBackMetaCopies);
	try {
		fs::rename(kMetadataTmpFilename, kMetadataFilename);
		safs_silent_syslog(LOG_DEBUG, "master.fs.stored");
		return true;
	} catch (Exception &ex) {
		safs_pretty_syslog(LOG_ERR, "renaming %s to %s failed: %s",
		                   kMetadataTmpFilename, kMetadataFilename, ex.what());
	}

	// The previous step didn't return, so let's try to save us in other way
	std::string alternativeName =
	    kMetadataFilename + std::to_string(eventloop_time());
	try {
		fs::rename(kMetadataTmpFilename, alternativeName);
		safs_pretty_syslog(LOG_ERR, "emergency metadata file created as %s",
		                   alternativeName.c_str());
		return false;
	} catch (Exception &ex) {
		safs_pretty_syslog(LOG_ERR, "renaming %s to %s failed: %s",
		                   kMetadataTmpFilename, alternativeName.c_str(),
		                   ex.what());
	}

	// Nothing can be done...
	safs_pretty_syslog_attempt(
	    LOG_ERR, "trying to create emergency metadata file in foreground");
	emergency_saves();
	return false;
}

int MetadataBackendFile::emergency_storeall(const std::string &fname) {
	cstream_t fd(fopen(fname.c_str(), "w"));
	if (fd == nullptr) {
		return -1;
	}

	store_fd(fd.get());

	if (ferror(fd.get()) != 0) {
		return -1;
	}
	safs_pretty_syslog(
	    LOG_WARNING,
	    "metadata were stored to emergency file: %s - please copy this file to "
	    "your default location as '%s'",
	    fname.c_str(), kMetadataFilename);
	return 0;
}

int MetadataBackendFile::emergency_saves() {
#if defined(SAUNAFS_HAVE_PWD_H) && defined(SAUNAFS_HAVE_GETPWUID)
	struct passwd *p;
#endif
	if (emergency_storeall(kMetadataEmergencyFilename) == 0) { return 0; }
#if defined(SAUNAFS_HAVE_PWD_H) && defined(SAUNAFS_HAVE_GETPWUID)
	p = getpwuid(getuid());
	if (p) {
		std::string fname = p->pw_dir;
		fname.append("/").append(kMetadataEmergencyFilename);
		if (emergency_storeall(fname) == 0) { return 0; }
	}
#endif
	std::string metadata_emergency_filename = kMetadataEmergencyFilename;
	if (emergency_storeall("/" + metadata_emergency_filename) == 0) {
		return 0;
	}
	if (emergency_storeall("/tmp/" + metadata_emergency_filename) == 0) {
		return 0;
	}
	if (emergency_storeall("/var/" + metadata_emergency_filename) == 0) {
		return 0;
	}
	if (emergency_storeall("/usr/" + metadata_emergency_filename) == 0) {
		return 0;
	}
	if (emergency_storeall("/usr/share/" + metadata_emergency_filename) == 0) {
		return 0;
	}
	if (emergency_storeall("/usr/local/" + metadata_emergency_filename) == 0) {
		return 0;
	}
	if (emergency_storeall("/usr/local/var/" + metadata_emergency_filename) ==
	    0) {
		return 0;
	}
	if (emergency_storeall("/usr/local/share/" + metadata_emergency_filename) ==
	    0) {
		return 0;
	}
	return -1;
}

void MetadataBackendFile::broadcast_metadata_saved(uint8_t status) {
	matomlserv_broadcast_metadata_saved(status);
	matoclserv_broadcast_metadata_saved(status);
}

uint8_t MetadataBackendFile::fs_storeall(DumpType dumpType) {
	if (gMetadata == nullptr) {
		// Periodic dump in shadow master or a request from saunafs-admin
		safs_pretty_syslog(LOG_INFO,
		                   "Can't save metadata because no metadata is loaded");
		return SAUNAFS_ERROR_NOTPOSSIBLE;
	}
	if (dumper()->inProgress()) {
		safs_pretty_syslog(LOG_ERR,
		                   "previous metadata save process hasn't finished yet "
		                   "- do not start another one");
		return SAUNAFS_ERROR_TEMP_NOTPOSSIBLE;
	}

	// We are going to do some changes in the data dir right now
	fs_erase_message_from_lockfile();
	changelog_rotate();
	matomlserv_broadcast_logrotate();
	// child == true says that we forked
	// bg may be changed to dump in foreground in case of a fork error
	bool child = dumper()->start(dumpType, fs_checksum(ChecksumMode::kGetCurrent));
	uint8_t status = SAUNAFS_STATUS_OK;

	if (dumpType == DumpType::kForegroundDump) {
		cstream_t fd(fopen(kMetadataTmpFilename, "w"));
		if (fd == nullptr) {
			safs_pretty_syslog(LOG_ERR, "can't open metadata file");
			// try to save in alternative location - just in case
			emergency_saves();
			if (child) {
				exit(1);
			}
			broadcast_metadata_saved(SAUNAFS_ERROR_IO);
			return SAUNAFS_ERROR_IO;
		}

		store_fd(fd.get());

		if (ferror(fd.get()) != 0) {
			safs_pretty_syslog(LOG_ERR, "can't write metadata");
			fd.reset();
			unlink(kMetadataTmpFilename);
			// try to save in alternative location - just in case
			emergency_saves();
			if (child) {
				exit(1);
			}
			broadcast_metadata_saved(SAUNAFS_ERROR_IO);
			return SAUNAFS_ERROR_IO;
		} else {
			if (fflush(fd.get()) == EOF) {
				safs_pretty_errlog(LOG_ERR, "metadata fflush failed");
			} else if (fsync(fileno(fd.get())) == -1) {
				safs_pretty_errlog(LOG_ERR, "metadata fsync failed");
			}
			fd.reset();
			if (!child) {
				// rename backups if no child was created, otherwise this is
				// handled by pollServe
				status = commit_metadata_dump() ? SAUNAFS_STATUS_OK
				                                : SAUNAFS_ERROR_IO;
			}
		}
		if (child) {
			printf("OK\n");  // give sfsmetarestore another chance
			safs::log_info("Child process for metadata dumping finished (pid: {})", getpid());
			exit(0);
		}
		broadcast_metadata_saved(status);
	}
	sassert(!child);
	return status;
}

#endif  // #if !defined(METARESTORE) && !defined(METALOGGER)

#ifndef METALOGGER

static bool xattr_load(MetadataLoader::Options options) {
	const uint8_t *ptr;
	inode_t inode = 0;
	uint8_t attributeNameLength = 0;
	uint32_t attributeValueLength = 0;
	XAttributeInodeEntry *xattrInodeEntry = nullptr;

	while (true) {
		xattrInodeEntry = nullptr;  // Reset pointer to avoid stale values

		try {
			ptr = options.metadataFile->seek(options.offset);
		} catch (const std::exception &e) {
			safs::log_exception(e, "loading xattr: can't read xattr");
			return false;
		}

		getINode(&ptr, inode);
		attributeNameLength = get8bit(&ptr);
		get32bit(&ptr, attributeValueLength);
		options.offset = options.metadataFile->offset(ptr);

		if (inode == 0) { return true; }

		if (attributeNameLength == 0) {
			safs::log_err("loading xattr: empty name");
			if (options.ignoreFlag) {
				continue;
			}
			return false;
		}

		if (attributeValueLength > SFS_XATTR_SIZE_MAX) {
			safs::log_err("loading xattr: value oversized");
			if (options.ignoreFlag) {
				continue;
			}
			return false;
		}

		auto inodeHash = get_xattr_inode_hash(inode);
		for (const auto &xattrEntry : gMetadata->xattrInodeHash[inodeHash]) {
			xattrInodeEntry = xattrEntry.get();
			if (xattrInodeEntry->inode == inode) {
				break;
			}
		}

		if (xattrInodeEntry != nullptr &&
		    xattrInodeEntry->attributeNameLength + attributeNameLength + 1 > SFS_XATTR_LIST_MAX) {
			safs::log_err("loading xattr: name list too long");
			if (options.ignoreFlag) {
				continue;
			}
			return false;
		}

		auto xattrEntry = std::make_unique<XAttributeDataEntry>();
		xattrEntry->inode = inode;
		xattrEntry->attributeName.resize(attributeNameLength);
		passert(xattrEntry->attributeName.data());
		memcpy(xattrEntry->attributeName.data(), ptr, attributeNameLength);
		ptr += attributeNameLength;

		if (attributeValueLength > 0) {
			xattrEntry->attributeValue.resize(attributeValueLength);
			passert(xattrEntry->attributeValue.data());
			memcpy(xattrEntry->attributeValue.data(), ptr, attributeValueLength);
			ptr += attributeValueLength;
		} else {
			xattrEntry->attributeValue.clear();
		}

		options.offset = options.metadataFile->offset(ptr);

		auto dataHash = get_xattr_data_hash(inode, xattrEntry->attributeName.size(),
		                                    xattrEntry->attributeName.data());

		gMetadata->xattrDataHash[dataHash].push_back(std::move(xattrEntry));
		auto *xattrEntryPointer = gMetadata->xattrDataHash[dataHash].back().get();

		if (xattrInodeEntry != nullptr) {
			xattrInodeEntry->xattrDataEntries.push_back(xattrEntryPointer);
			xattrInodeEntry->attributeNameLength += attributeNameLength + 1U;
			xattrInodeEntry->attributeValueLength += attributeValueLength;
		} else {
			auto xattrInodeEntry =
			    XAttributeInodeEntry::create(inode, attributeNameLength + 1U, attributeValueLength);
			xattrInodeEntry->xattrDataEntries.push_back(xattrEntryPointer);
			gMetadata->xattrInodeHash[inodeHash].push_back(std::move(xattrInodeEntry));
		}
	}
}

// TODO (Baldor): Despite the name, below function is called only once
//               so the value of polymorphism is questionable here
//               it seems a candidate for refactoring.
template <class... Args>
static bool fs_load_generic(const std::shared_ptr<MemoryMappedFile> &metadataFile,
                            size_t& offsetBegin, Args &&...args) {
	constexpr uint8_t kSize = sizeof(std::uint32_t);
	uint32_t size = 0;
	uint8_t *ptr;
	try {
		ptr = metadataFile->seek(offsetBegin);
	} catch (const std::exception &e) {
		safs_pretty_syslog(LOG_ERR, "loading node: %s", e.what());
		throw Exception("fread error (size)");
	}
	deserialize(ptr, kSize, size);
	offsetBegin += kSize;
	if (size == 0) {
		// marker
		return false;
	}
	try {
		ptr = metadataFile->seek(offsetBegin);
	} catch (const std::exception &e) {
		safs_pretty_syslog(LOG_ERR, "loading node: %s", e.what());
		throw Exception("fread error (size)");
	}
	deserialize(ptr, size, std::forward<Args>(args)...);
	offsetBegin += size;
	return true;
}

/**
 * @brief Parse an edge from the metadata file.
 * @param metadataFile A reference to the memory mapped metadata file.
 * @param sectionOffset A reference to point to the next edge attribute.
 * @param ignoreFlag A flag to indicate whether to ignore the error.
 * @param init A flag to indicate whether to initialize the static variable.
 * @return 0 on success, 1 if last edge mark is found, -1 if unknown node type.
 */
static int8_t fs_parseEdge(const std::shared_ptr<MemoryMappedFile> &metadataFile,
                           size_t &sectionOffset, int ignoreFlag, bool init = false) {
	static const int8_t kError = -1;
	static const int8_t kSuccess = 0;
	static const int8_t kLastEdge = 1;
	static inode_t currentParentId;
	if (init) {
		currentParentId = 0;
		return kSuccess;
	}
	const auto* pSrc = metadataFile->seek(sectionOffset);
	inode_t parentId;
	getINode(&pSrc, parentId);
	inode_t childId;
	getINode(&pSrc, childId);
	auto edgeNameSize = get16bit(&pSrc);
	sectionOffset = metadataFile->offset(pSrc);

	if (!parentId && !childId) {  /// Last edge mark;
		return kLastEdge;
	}

	if (!edgeNameSize) {
		safs_pretty_syslog(
		    LOG_ERR, "loading edge: %" PRIiNode "->%" PRIiNode " error: empty name",
		    parentId, childId);
		return kError;
	}

	std::string name(pSrc, pSrc + edgeNameSize);
	sectionOffset += edgeNameSize;
	FSNode *child = fsnodes_id_to_node(childId);
	if (!child) {
		safs_pretty_syslog(
		    LOG_ERR,
		    "loading edge: %" PRIiNode ",%s->%" PRIiNode " error: child not found",
		    parentId, fsnodes_escape_name(name).c_str(), childId);
		if (ignoreFlag) {
			return kSuccess;
		}
		return kError;
	}
	if (!parentId) {
		if (child->type == FSNodeType::kTrash) {
			gMetadata->trash.insert(
			    {TrashPathKey(child), hstorage::Handle(name)});
			gMetadata->trashSpace += static_cast<FSNodeFile *>(child)->length;
			gMetadata->trashNodes++;
		} else if (child->type == FSNodeType::kReserved) {
			gMetadata->reserved.insert({child->id, hstorage::Handle(name)});
			gMetadata->reservedSpace += static_cast<FSNodeFile *>(child)->length;
			gMetadata->reservedNodes++;
		} else {
			safs::log_err("loading edge: {}, {}->{} error: bad child type ({})", parentId,
			              fsnodes_escape_name(name), childId, static_cast<char>(child->type));
			return kError;
		}
	} else {
		FSNodeDirectory *parent = fsnodes_id_to_node<FSNodeDirectory>(parentId);
		if (!parent) {
			safs_pretty_syslog(LOG_ERR,
			                   "loading edge: %" PRIiNode ",%s->%" PRIiNode
			                   " error: parent not found",
			                   parentId, fsnodes_escape_name(name).c_str(),
			                   childId);
			if (ignoreFlag) {
				parent =
				    fsnodes_id_to_node<FSNodeDirectory>(SPECIAL_INODE_ROOT);
				if (!parent || parent->type != FSNodeType::kDirectory) {
					safs_pretty_syslog(
					    LOG_ERR,
					    "loading edge: %" PRIiNode ",%s->%" PRIiNode
					    " root dir not found !!!",
					    parentId, fsnodes_escape_name(name).c_str(), childId);
					return kError;
				}
				safs_pretty_syslog(LOG_ERR,
				                   "loading edge: %" PRIiNode ",%s->%" PRIiNode
				                   " attaching node to root dir",
				                   parentId, fsnodes_escape_name(name).c_str(),
				                   childId);
				parentId = SPECIAL_INODE_ROOT;
			} else {
				safs_pretty_syslog(
				    LOG_ERR,
				    "use sfsmetarestore (option -i) to attach this "
				    "node to root dir\n");
				return kError;
			}
		}
		if (parent->type != FSNodeType::kDirectory) {
			safs::log_err("loading edge: {}, {}->{} error: bad parent type ({})", parentId,
			              fsnodes_escape_name(name), childId, static_cast<char>(parent->type));
			if (ignoreFlag) {
				parent =
				    fsnodes_id_to_node<FSNodeDirectory>(SPECIAL_INODE_ROOT);
				if (!parent || parent->type != FSNodeType::kDirectory) {
					safs_pretty_syslog(
					    LOG_ERR,
					    "loading edge: %" PRIiNode ",%s->%" PRIiNode
					    " root dir not found !!!",
					    parentId, fsnodes_escape_name(name).c_str(), childId);
					return kError;
				}
				safs_pretty_syslog(LOG_ERR,
				                   "loading edge: %" PRIiNode ",%s->%" PRIiNode
				                   " attaching node to root dir",
				                   parentId, fsnodes_escape_name(name).c_str(),
				                   childId);
				parentId = SPECIAL_INODE_ROOT;
			} else {
				safs_pretty_syslog(
				    LOG_ERR,
				    "use sfsmetarestore (option -i) to attach this "
				    "node to root dir\n");
				return kError;
			}
		}
		if (currentParentId != parentId) {
			if (parent->entries.size() > 0) {
				safs_pretty_syslog(LOG_ERR,
				                   "loading edge: %" PRIiNode ",%s->%" PRIiNode
				                   " error: parent node sequence error",
				                   parentId, fsnodes_escape_name(name).c_str(),
				                   childId);
				return kError;
			}
			currentParentId = parentId;
		}

		hstorage::Handle *handlePtr = new hstorage::Handle(name);
		auto it = parent->entries.insert({handlePtr, child}).first;
		parent->entries_hash ^= (*it).first->hash();

		if (parent->case_insensitive) {
			HString lowerCaseName = HString::hstringToLowerCase(HString(name));
			auto lowercaseHandlePtr = new hstorage::Handle(lowerCaseName);
			auto it =
			    parent->lowerCaseEntries.insert({lowercaseHandlePtr, child})
			        .first;
			parent->lowerCaseEntriesHash ^= (*it).first->hash();
		}

		child->parent.push_back({parent->id, handlePtr});
		if (child->type == FSNodeType::kDirectory) {
			parent->nlink++;
		}

		statsrecord sr;
		fsnodes_get_stats(child, &sr);
		fsnodes_add_stats(parent, &sr);
	}
	return kSuccess;
}

/**
 * @brief
 * @param pSrc A pointer to the data storing all nodes.
 * @param sectionOffset A reference to point to the next node attribute.
 * @return 0 on success, 1 if last node mark is found, -1 if unknown node type.
 */
static int8_t fs_parseNode(const std::shared_ptr<MemoryMappedFile> &metadataFile,
                           size_t &sectionOffset) {
	static const int8_t kError = -1;
	static const int8_t kSuccess = 0;
	static const int8_t kLastNode = 1;

	const uint8_t *pSrc = metadataFile->seek(sectionOffset);
	uint8_t typeU8 = get8bit(&pSrc);

	if (!typeU8) {
		sectionOffset = metadataFile->offset(pSrc);
		return kLastNode;
	}

	auto type = static_cast<FSNodeType>(typeU8);
	FSNode *node = FSNode::create(type);
	passert(node);
	getINode(&pSrc, node->id);
	node->goal = get8bit(&pSrc);
	node->mode = get16bit(&pSrc);
	get32bit(&pSrc, node->uid);
	get32bit(&pSrc, node->gid);
	get32bit(&pSrc, node->atime);
	get32bit(&pSrc, node->mtime);
	get32bit(&pSrc, node->ctime);
	get32bit(&pSrc, node->trashtime);
	sectionOffset = metadataFile->offset(pSrc);
	auto *nodeFile = static_cast<FSNodeFile *>(node);

	uint32_t nodeNameLength;
	uint32_t chunkAmount;
	uint16_t sessionIds;

	uint32_t index;
	constexpr uint32_t kChunkSize = (1 << 16);

	switch (type) {
	case FSNodeType::kDirectory:
		gMetadata->dirNodes++;
		break;
	case FSNodeType::kSocket:
	case FSNodeType::kFifo:  /// No extra info to Parse
		break;
	case FSNodeType::kBlockDev:
	case FSNodeType::kCharDev:
		uint32_t tempRDev;
		get32bit(&pSrc, tempRDev);
		static_cast<FSNodeDevice *>(node)->rdev = tempRDev;
		break;
	case FSNodeType::kSymlink:
		get32bit(&pSrc, nodeNameLength);
		static_cast<FSNodeSymlink *>(node)->path_length = nodeNameLength;
		if (nodeNameLength > 0) {
			static_cast<FSNodeSymlink *>(node)->path = HString(pSrc, pSrc + nodeNameLength);
			pSrc += nodeNameLength;
		}
		gMetadata->linkNodes++;
		break;
	case FSNodeType::kFile:
	case FSNodeType::kTrash:
	case FSNodeType::kReserved:
		nodeFile->length = get64bit(&pSrc);
		get32bit(&pSrc, chunkAmount);
		sessionIds = get16bit(&pSrc);

		nodeFile->chunks.resize(chunkAmount);

		index = 0;
		while (chunkAmount > kChunkSize) {
			for (uint32_t i = 0; i < kChunkSize; i++) {
				nodeFile->chunks[index++] = get64bit(&pSrc);
			}
			chunkAmount -= kChunkSize;
		}
		for (uint32_t i = 0; i < chunkAmount; i++) {
			nodeFile->chunks[index++] = get64bit(&pSrc);
		}
		while (sessionIds) {
			uint32_t sessionId;
			get32bit(&pSrc, sessionId);
			nodeFile->sessionid.push_back(sessionId);
#ifndef METARESTORE
			matoclserv_add_open_file(sessionId, node->id);
#endif
			sessionIds--;
		}

		fsnodes_quota_update(node, {{QuotaResource::kSize, +fsnodes_get_size(node)}});
		gMetadata->fileNodes++;
		break;
	default:
		safs::log_err("loading node: unrecognized node type: {}", static_cast<char>(type));
		fsnodes_quota_update(node, {{QuotaResource::kInodes, +1}});
		sectionOffset = metadataFile->offset(pSrc);
		return kError;
	}

	uint32_t nodeHashIndex = NODEHASHPOS(node->id);
	gMetadata->nodeHash[nodeHashIndex].push_back(node);

	gMetadata->inodePool.markAsAcquired(node->id);
	gMetadata->nodes++;
	fsnodes_quota_update(node, {{QuotaResource::kInodes, +1}});
	sectionOffset = metadataFile->offset(pSrc);
	return kSuccess;
}

static int fs_lostnode(FSNode *p) {
	uint8_t artname[40];
	uint32_t i, l;
	i = 0;
	do {
		if (i == 0) {
			l = snprintf((char *)artname, 40, "lost_node_%" PRIiNode, p->id);
		} else {
			l = snprintf((char *)artname, 40, "lost_node_%" PRIiNode ".%" PRIu32,
			             p->id, i);
		}
		HString name((const char *)artname, l);
		if (!fsnodes_nameisused(gMetadata->root, name)) {
			fsnodes_link(0, gMetadata->root, p, name);
			return 1;
		}
		i++;
	} while (i);
	return -1;
}

int fs_checknodes(int ignoreflag) {
	for (auto i = 0; i < NODEHASHSIZE; i++) {
		for (const auto &node : gMetadata->nodeHash[i]) {
			if (node->parent.empty() && node != gMetadata->root &&
			    (node->type != FSNodeType::kTrash) && (node->type != FSNodeType::kReserved)) {
				safs::log_err("found orphaned inode: %" PRIiNode, node->id);
				if (ignoreflag) {
					if (fs_lostnode(node) < 0) {
						return -1;
					}
				} else {
					safs::log_err("use sfsmetarestore (option -i) to attach this node to root dir");
					return -1;
				}
			}
		}
	}
	return 1;
}

static bool fs_loadnodes(MetadataLoader::Options options) {
	int8_t status;
	do {
		status = fs_parseNode(options.metadataFile, options.offset);
		if (status < 0) {
			return false;
		}
	} while (status == 0);
	return true;
}

static bool fs_loadedges(MetadataLoader::Options options) {
	int8_t status;
	fs_parseEdge(options.metadataFile, options.offset, options.ignoreFlag, true);
	do {
		status = fs_parseEdge(options.metadataFile, options.offset, options.ignoreFlag);
		if (status < 0) {
			return false;
		}
	} while (status == 0);
	return true;
}

static bool fs_loadquotas(MetadataLoader::Options options) {
	try {
		std::vector<QuotaEntry> entries;
		fs_load_generic(options.metadataFile, options.offset, entries);
		for (const auto &entry : entries) {
			gMetadata->quotaDatabase.set(
			    entry.entryKey.owner.ownerType, entry.entryKey.owner.ownerId,
			    entry.entryKey.rigor, entry.entryKey.resource, entry.limit);
		}
		gMetadata->quotaChecksum = gMetadata->quotaDatabase.checksum();
	} catch (Exception &ex) {
		safs_pretty_syslog(LOG_ERR, "loading quotas: %s", ex.what());
		if (!options.ignoreFlag || ex.status() != SAUNAFS_STATUS_OK) {
			return false;
		}
	}
	return true;
}

static bool fs_loadlocks(MetadataLoader::Options options) {
	try {
		gMetadata->flockLocks.load(options.metadataFile, options.offset);
		gMetadata->posixLocks.load(options.metadataFile, options.offset);
	} catch (Exception &ex) {
		safs_pretty_syslog(LOG_ERR, "loading locks: %s", ex.what());
		if (!options.ignoreFlag || ex.status() != SAUNAFS_STATUS_OK) {
			return false;
		}
	}
	return true;
}

bool fs_loadfree(MetadataLoader::Options options) {
	const uint8_t *ptr;
	inode_t freeNodesToLoad;
	inode_t freeNodesNumber;
	uint32_t timestamp;

	try {
		ptr = options.metadataFile->seek(options.offset);
	} catch (const std::exception &e) {
		safs_pretty_errlog(LOG_INFO, "loading free nodes: %s", e.what());
		return false;
	}

	getINode(&ptr, freeNodesNumber);

	constexpr uint8_t kFreeNodesEntrySize = sizeof(inode_t) + sizeof(timestamp);

	if (options.sectionLength &&
	    freeNodesNumber != (options.sectionLength - sizeof(inode_t)) / kFreeNodesEntrySize) {
		safs_pretty_errlog(LOG_INFO,
		                   "loading free nodes: section size doesn't match "
		                   "number of free nodes");
		freeNodesNumber = (options.sectionLength - sizeof(inode_t)) / kFreeNodesEntrySize;
	}

	freeNodesToLoad = 0;

	constexpr inode_t kFreeBatchSize = 1024;
	inode_t inode{0};

	while (freeNodesNumber > 0) {
		if (freeNodesToLoad == 0) {
			freeNodesToLoad = std::min(freeNodesNumber, kFreeBatchSize);
		}
		getINode(&ptr, inode);
		get32bit(&ptr, timestamp);
		gMetadata->inodePool.detain(inode, timestamp, true);
		freeNodesToLoad--;
		freeNodesNumber--;
	}
	options.offset = options.metadataFile->offset(ptr);
	return true;
}

static constexpr uint8_t kMetadataSectionHeaderSize = 16;
static constexpr uint8_t kMetadataSectionNameSize = 8;

static const std::vector<MetadataSection> kMetadataSections = {
    /// Synchronously loaded sections (in order)
    MetadataSection("NODE 1.0", "Nodes", fs_loadnodes, false),
    /// Asynchronously loaded sections:
    MetadataSection("EDGE 1.0", "Edges", fs_loadedges),
    MetadataSection("FREE 1.0", "Free Nodes", fs_loadfree),
    MetadataSection("XATR 1.0", "Extended Attributes", xattr_load),
    MetadataSection("ACLS 1.0", "Access Control Lists", fs_load_legacy_acls),
    MetadataSection("ACLS 1.1", "Access Control Lists", fs_load_posix_acls),
    MetadataSection("ACLS 1.2", "Access Control Lists", fs_load_acls),
    MetadataSection("QUOT 1.1", "Quotas", fs_loadquotas),
    MetadataSection("FLCK 1.0", "File Locks", fs_loadlocks),
    MetadataSection("CHNK 1.0", "Chunks", chunksLoadFromFile),
    /// Legacy Sections (won't be loaded):
    MetadataSection("QUOT 1.0", "Quotas",
                    [](const MetadataLoader::Options &) { return true; }, true, true),
    MetadataSection("LOCK 1.0", "Locks",
                    [](const MetadataLoader::Options &) { return true; }, true, true),
};

static bool isEndOfMetadata(const uint8_t *sectionPtr) {
	static constexpr std::string_view kMetadataTrailer("[" SFSSIGNATURE " EOF MARKER]");
	static constexpr std::string_view kMetadataLegacyTrailer("[MFS EOF MARKER]");
	return ((memcmp(sectionPtr, kMetadataTrailer.data(),
	              ::kMetadataSectionHeaderSize) == kOpSuccess) ||
	        (memcmp(sectionPtr, kMetadataLegacyTrailer.data(),
	              ::kMetadataSectionHeaderSize) == kOpSuccess));
}

static int fs_load(const std::shared_ptr<MemoryMappedFile> &metadataFile, int ignoreflag) {
	static constexpr uint8_t kMetadataHeaderOffset = 8;

	/// Skip File Signature
	const uint8_t *metadataHeaderPtr= metadataFile->seek(kMetadataHeaderOffset);

	getINode(&metadataHeaderPtr, gMetadata->maxInodeId);
	gMetadata->metadataVersion = get64bit(&metadataHeaderPtr);
	get32bit(&metadataHeaderPtr, gMetadata->nextSessionId);

	size_t offsetBegin = metadataFile->offset(metadataHeaderPtr);

	/// First secuential pass to gather section offsets and lengths
	std::unordered_map<std::string_view, std::pair<size_t, uint64_t>> sectionMarkers;
	uint8_t *sectionPtr = metadataFile->seek(offsetBegin);
	while (!isEndOfMetadata(sectionPtr)) {
		const uint8_t *sectionLengthPtr = sectionPtr + kMetadataSectionNameSize;
		uint64_t sectionLength = get64bit(&sectionLengthPtr);
		const uint8_t *sectionDataPtr = sectionLengthPtr;
		for (const auto &section : kMetadataSections) {
			if (section.matchesSectionTypeOf(sectionPtr)) {
				sectionMarkers[section.name] = {metadataFile->offset(sectionDataPtr),
				                                sectionLength};
				break;
			}
		}
		sectionPtr = metadataFile->seek(
		    offsetBegin += sectionLength + kMetadataSectionHeaderSize);
	}

	MetadataLoader::Futures futures;
	for (const auto &section : kMetadataSections) {
		if (sectionMarkers.find(section.name) == sectionMarkers.end()) {
			continue;
		}
		size_t sectionOffset = sectionMarkers[section.name].first;
		uint64_t sectionLength = sectionMarkers[section.name].second;
		if (section.isLegacy) {
			safs_pretty_syslog(LOG_WARNING, "legacy section found (%s)",
			                   section.name.data());
			continue;
		}
		auto options = MetadataLoader::Options{metadataFile, sectionOffset,
		                                       ignoreflag, sectionLength, true};

		if (!section.asyncLoad) {
			MetadataLoader::loadSection(section, options);
		} else {
			MetadataLoader::loadSectionAsync(section, options, futures);
		}
	}
	/// Wait for all futures to finish and exit if any of them failed
	bool success = true;
	for (auto &future : futures) {
		future.future.wait();
		success &= future.future.get();
	}

	if (!success) {
		return kOpFailure;
	}
	safs_pretty_syslog_attempt(
	    LOG_INFO, "checking filesystem consistency of the metadata file");
	fflush(stderr);
	gMetadata->root = fsnodes_id_to_node<FSNodeDirectory>(SPECIAL_INODE_ROOT);
	if (gMetadata->root == nullptr) {
		safs_pretty_syslog(LOG_ERR,
		                   "error reading metadata (root node not found)");
		return kOpFailure;
	}
	if (gMetadata->root->type != FSNodeType::kDirectory) {
		safs_pretty_syslog(
		    LOG_ERR, "error reading metadata (root node not a directory)");
		return kOpFailure;
	}
	if (fs_checknodes(ignoreflag) < 0) {
		return kOpFailure;
	}
	return kOpSuccess;
}

#ifndef METARESTORE

void fs_new(void) {
	gMetadata->maxInodeId = SPECIAL_INODE_ROOT;
	gMetadata->metadataVersion = 1;
	gMetadata->nextSessionId = 1;

	auto *rootDirectory = FSNode::create(FSNodeType::kDirectory);
	gMetadata->root = static_cast<FSNodeDirectory *>(rootDirectory);
	gMetadata->root->id = SPECIAL_INODE_ROOT;
	gMetadata->root->atime = eventloop_time();
	gMetadata->root->mtime = gMetadata->root->atime;
	gMetadata->root->ctime = gMetadata->root->mtime;
	gMetadata->root->goal = DEFAULT_GOAL;
	gMetadata->root->trashtime = kDefaultTrashTime;
	gMetadata->root->mode = 0777;
	gMetadata->root->uid = 0;
	gMetadata->root->gid = 0;

	uint32_t hashRootIndex = NODEHASHPOS(gMetadata->root->id);
	gMetadata->nodeHash[hashRootIndex].push_back(gMetadata->root);
	gMetadata->inodePool.markAsAcquired(gMetadata->root->id);

	chunk_newfs();

	gMetadata->nodes = 1;
	gMetadata->dirNodes = 1;
	gMetadata->fileNodes = 0;

	fs_checksum(ChecksumMode::kForceRecalculate);
	fsnodes_quota_update(gMetadata->root, {{QuotaResource::kInodes, +1}});
}

#endif

bool isNewMetadataFile([[maybe_unused]]const uint8_t *headerPtr) {
	[[maybe_unused]] static constexpr std::string_view kMetadataHeaderNew(
	    SFSSIGNATURE "M NEW");
	[[maybe_unused]] static constexpr std::string_view kMetadataHeaderOld(
	    SAUSIGNATURE "M NEW");
	[[maybe_unused]]static constexpr uint8_t kMetadataHeaderSize = 8;
#ifndef METARESTORE
	if (metadataserver::isMaster()) {  // special case - create new file system
		if ((memcmp(headerPtr, kMetadataHeaderNew.data(), kMetadataHeaderSize) == kOpSuccess) ||
		    (memcmp(headerPtr, kMetadataHeaderOld.data(), kMetadataHeaderSize) == kOpSuccess)){
			fs_new();
			safs_pretty_syslog(LOG_NOTICE, "empty filesystem created");
			// after creating new filesystem always create "back" file for using
			// in metarestore
			gMetadataBackend->fs_storeall(DumpType::kForegroundDump);
			return true;
		}
	}
#endif /* #ifndef METARESTORE */
	return false;
}

static bool checkMetadataSignature(const std::shared_ptr<MemoryMappedFile> &metadataFile) {
	static constexpr std::string_view kMetadataHeaderNewV2_9(SFSSIGNATURE "M 2.9");
	static constexpr std::string_view kMetadataHeaderOldV2_9(SAUSIGNATURE "M 2.9");
	static constexpr std::string_view kMetadataHeaderLegacy("LIZM 2.9");
	static constexpr uint8_t kMetadataHeaderSize = 8;
	size_t kMetadataHeaderOffset{0};
	uint8_t *headerPtr;
	try {
		headerPtr = metadataFile->seek(kMetadataHeaderOffset);
		safs_pretty_syslog(LOG_INFO, "opened metadata file %s",
		                   metadataFile->filename().c_str());
	} catch (const std::exception &e) {
		throw e;
	}
	if (isNewMetadataFile(headerPtr)) {
		return false;
	}
	if ((memcmp(headerPtr, kMetadataHeaderNewV2_9.data(), kMetadataHeaderSize) != kOpSuccess) &&
	    (memcmp(headerPtr, kMetadataHeaderOldV2_9.data(), kMetadataHeaderSize) != kOpSuccess) &&
	    (memcmp(headerPtr, kMetadataHeaderLegacy.data(), kMetadataHeaderSize) != kOpSuccess)) {
		throw MetadataConsistencyException("wrong metadata header version");
	}
	return true;
}

void MetadataBackendFile::loadall(int ignoreflag) {
	auto metadataFile = std::make_shared<MemoryMappedFile>(metadataFile_);
	if (!checkMetadataSignature(metadataFile)) { return; }
	if (fs_load(metadataFile, ignoreflag) != kOpSuccess) {
		throw MetadataConsistencyException(MetadataStructureReadErrorMsg);
	}
	safs_pretty_syslog_attempt(LOG_INFO, "connecting files and chunks");
	fs_add_files_to_chunks();
	unlink(kMetadataTmpFilename);
	safs_pretty_syslog_attempt(LOG_INFO,
	                           "calculating checksum of the metadata");
	fs_checksum(ChecksumMode::kForceRecalculate);

#ifndef METARESTORE
	safs_pretty_syslog(
	    LOG_INFO,
	    "metadata file %s read (%" PRIiNode " inodes including %" PRIiNode
	    " directory inodes, %" PRIiNode " file inodes, %" PRIiNode
	    " symlink inodes and %" PRIu32 " chunks)",
	    metadataFile->filename().c_str(), gMetadata->nodes, gMetadata->dirNodes,
	    gMetadata->fileNodes, gMetadata->linkNodes, chunk_count());
#else
	safs::log_info("metadata file {} read", metadataFile_);
#endif
}

//Nodes

void MetadataBackendFile::storenode(FSNode *f, FILE *fd) {
	uint8_t *ptr, *chptr;
	uint32_t i, indx, ch, sessionids;
	std::string name;

	if (f == NULL) {  // last node
		fputc(0, fd);
		return;
	}

	ptr = gNodeStoreBuffer;
	put8bit(&ptr, static_cast<uint8_t>(f->type));
	putINode(&ptr, f->id);
	put8bit(&ptr, f->goal);
	put16bit(&ptr, f->mode);
	put32bit(&ptr, f->uid);
	put32bit(&ptr, f->gid);
	put32bit(&ptr, f->atime);
	put32bit(&ptr, f->mtime);
	put32bit(&ptr, f->ctime);
	put32bit(&ptr, f->trashtime);

	auto *node_file = static_cast<FSNodeFile *>(f);

	switch (f->type) {
	case FSNodeType::kDirectory:
	case FSNodeType::kSocket:
	case FSNodeType::kFifo:
		if (fwrite(gNodeStoreBuffer, 1, kNodeHeaderSize, fd) !=
		    (size_t)(kNodeHeaderSize)) {
			safs_pretty_syslog(LOG_NOTICE, "fwrite error");
			return;
		}
		break;
	case FSNodeType::kBlockDev:
	case FSNodeType::kCharDev:
		put32bit(&ptr, static_cast<FSNodeDevice *>(f)->rdev);
		if (fwrite(gNodeStoreBuffer, 1, kNodeHeaderSize + 4, fd) !=
		    (size_t)(kNodeHeaderSize + 4)) {
			safs_pretty_syslog(LOG_NOTICE, "fwrite error");
			return;
		}
		break;
	case FSNodeType::kSymlink:
		name = (std::string) static_cast<FSNodeSymlink *>(f)->path;
		// Safe cast, the length should always fit
		put32bit(&ptr, static_cast<uint32_t>(name.length()));
		if (fwrite(gNodeStoreBuffer, 1, kNodeHeaderSize + 4, fd) !=
		    (size_t)(kNodeHeaderSize + 4)) {
			safs_pretty_syslog(LOG_NOTICE, "fwrite error");
			return;
		}
		if (fwrite(name.c_str(), 1, name.length(), fd) !=
		    (size_t)(static_cast<FSNodeSymlink *>(f)->path_length)) {
			safs_pretty_syslog(LOG_NOTICE, "fwrite error");
			return;
		}
		break;
	case FSNodeType::kFile:
	case FSNodeType::kTrash:
	case FSNodeType::kReserved:
		put64bit(&ptr, node_file->length);
		ch = node_file->chunkCount();
		put32bit(&ptr, ch);
		sessionids =
		    std::min<int>(node_file->sessionid.size(), kMaxSessionSize);
		put16bit(&ptr, sessionids);

		if (fwrite(gNodeStoreBuffer, 1,
		           kNodeHeaderSize + kFileSpecificHeaderSize,
		           fd) != (size_t)(kNodeHeaderSize + kFileSpecificHeaderSize)) {
			safs_pretty_syslog(LOG_NOTICE, "fwrite error");
			return;
		}

		indx = 0;
		while (ch > kChunksBucketSize) {
			chptr = ptr;
			for (i = 0; i < kChunksBucketSize; i++) {
				put64bit(&chptr, node_file->chunks[indx]);
				indx++;
			}
			if (fwrite(ptr, 1, 8 * kChunksBucketSize, fd) !=
			    (size_t)(8 * kChunksBucketSize)) {
				safs_pretty_syslog(LOG_NOTICE, "fwrite error");
				return;
			}
			ch -= kChunksBucketSize;
		}

		chptr = ptr;
		for (i = 0; i < ch; i++) {
			put64bit(&chptr, node_file->chunks[indx]);
			indx++;
		}

		sessionids = 0;
		for (const auto &sid : node_file->sessionid) {
			if (sessionids >= kMaxSessionSize) {
				break;
			}
			put32bit(&chptr, sid);
			sessionids++;
		}

		if (fwrite(ptr, 1, 8 * ch + 4 * sessionids, fd) !=
		    (size_t)(8 * ch + 4 * sessionids)) {
			safs_pretty_syslog(LOG_NOTICE, "fwrite error");
			return;
		}
		break;
	default:
		safs::log_err("MetadataBackendFile::storenode: unexpected node type {}",
		              static_cast<char>(f->type));
		break;
	}
}

void MetadataBackendFile::storenodes(FILE *fd) {
	for (uint32_t i = 0; i < NODEHASHSIZE; i++) {
		for (const auto &node : gMetadata->nodeHash[i]) {
			storenode(node, fd);
		}
	}
	storenode(nullptr, fd);  // end marker
}

//Edges

void MetadataBackendFile::storeedge(FSNodeDirectory *parent, FSNode *child,
                                    const std::string &name, FILE *fd) {
	uint8_t *ptr;
	if (child == nullptr) {  // last edge
		memset(gEdgeStoreBuffer, 0, kEdgeHeaderSize);
		if (fwrite(gEdgeStoreBuffer, 1, kEdgeHeaderSize, fd) !=
		    (size_t)(kEdgeHeaderSize)) {
			safs_pretty_syslog(LOG_NOTICE, "fwrite error");
			return;
		}
		return;
	}
	ptr = gEdgeStoreBuffer;
	putINode(&ptr, (parent == nullptr) ? 0 : parent->id);
	putINode(&ptr, child->id);
	put16bit(&ptr, name.length());
	memcpy(ptr, name.c_str(), name.length());
	if (fwrite(gEdgeStoreBuffer, 1, kEdgeHeaderSize + name.length(), fd) !=
	    (size_t)(kEdgeHeaderSize + name.length())) {
		safs_pretty_syslog(LOG_NOTICE, "fwrite error");
		return;
	}
}

void MetadataBackendFile::storeedgelist(FSNodeDirectory *parent, FILE *fd) {
	for (const auto &entry : parent->entries) {
		storeedge(parent, entry.second, (std::string)(*entry.first), fd);
	}
}

void MetadataBackendFile::storeedgelist(const TrashPathContainer &data, FILE *fd) {
	for (const auto &entry : data) {
		FSNode *child = fsnodes_id_to_node(entry.first.id);
		storeedge(nullptr, child, (std::string)entry.second, fd);
	}
}

void MetadataBackendFile::storeedgelist(const ReservedPathContainer &data, FILE *fd) {
	for (const auto &entry : data) {
		FSNode *child = fsnodes_id_to_node(entry.first);
		storeedge(nullptr, child, (std::string)entry.second, fd);
	}
}

void MetadataBackendFile::storeedges_rec(FSNodeDirectory *f, FILE *fd) {
	storeedgelist(f, fd);
	for (const auto &entry : f->entries) {
		if (entry.second->type == FSNodeType::kDirectory) {
			storeedges_rec(static_cast<FSNodeDirectory *>(entry.second), fd);
		}
	}
}

void MetadataBackendFile::storeedges(FILE *fd) {
	storeedges_rec(gMetadata->root, fd);
	storeedgelist(gMetadata->trash, fd);
	storeedgelist(gMetadata->reserved, fd);
	storeedge(nullptr, nullptr, std::string(), fd);  // end marker
}

//FREE

void MetadataBackendFile::storefree(FILE *fd) {
	constexpr uint32_t kFreeBatchSize = 1024;
	constexpr size_t kFreeEntrySize = sizeof(inode_t) + sizeof(uint32_t);  // inode + timestamp
	constexpr size_t kFreeFullBatchSize = kFreeBatchSize * kFreeEntrySize;

	uint8_t wbuff[kFreeFullBatchSize], *ptr;

	inode_t totalFreeNodes = gMetadata->inodePool.detainedCount();

	ptr = wbuff;
	putINode(&ptr, totalFreeNodes);

	if (fwrite(wbuff, 1, sizeof(inode_t), fd) != sizeof(inode_t)) {
		safs_pretty_syslog(LOG_NOTICE, "fwrite error");
		return;
	}

	uint32_t batchCursor = 0;
	ptr = wbuff;

	for (const auto &n : gMetadata->inodePool) {
		if (batchCursor == kFreeBatchSize) {
			if (fwrite(wbuff, 1, kFreeFullBatchSize, fd) != kFreeFullBatchSize) {
				safs_pretty_syslog(LOG_NOTICE, "fwrite error");
				return;
			}

			batchCursor = 0;
			ptr = wbuff;
		}

		putINode(&ptr, n.id);
		put32bit(&ptr, n.ts);
		batchCursor++;
	}

	if (batchCursor > 0) {
		if (fwrite(wbuff, 1, kFreeEntrySize * batchCursor, fd) != kFreeEntrySize * batchCursor) {
			safs_pretty_syslog(LOG_NOTICE, "fwrite error");
			return;
		}
	}
}

// XAttr

void MetadataBackendFile::xattr_store(FILE *fd) {
	constexpr uint8_t kXAttributeNameLengthSize = 0;
	constexpr uint32_t kXAttributeValueLengthSize = 0;
	constexpr uint32_t kHdrSize = kinode_t_size + sizeof(kXAttributeNameLengthSize) +
	                              sizeof(kXAttributeValueLengthSize);
	uint8_t headerBuffer[kHdrSize];
	uint8_t *ptr;

	for (auto i = 0; i < XATTR_DATA_HASH_SIZE; i++) {
		for (const auto &xattrDataEntry : gMetadata->xattrDataHash[i]) {
			ptr = headerBuffer;
			putINode(&ptr, xattrDataEntry->inode);
			put8bit(&ptr, xattrDataEntry->attributeName.size());
			put32bit(&ptr, static_cast<uint32_t>(xattrDataEntry->attributeValue.size()));

			if (fwrite(headerBuffer, 1, kHdrSize, fd) != (size_t)(kHdrSize)) {
				safs::log_info("{}: fwrite error failed writing header", __func__);
				return;
			}

			auto *attributeNameData = xattrDataEntry->attributeName.data();
			auto nameLength = xattrDataEntry->attributeName.size();
			if (fwrite(attributeNameData, 1, nameLength, fd) != (size_t)(nameLength)) {
				safs::log_info("{}: fwrite error failed writing attribute name", __func__);
				return;
			}

			auto *attributeValueData = xattrDataEntry->attributeValue.data();
			auto valueLength = xattrDataEntry->attributeValue.size();
			if (valueLength > 0) {
				if (fwrite(attributeValueData, 1, valueLength, fd) != (size_t)(valueLength)) {
					safs::log_info("{}: fwrite error failed writing attribute value", __func__);
					return;
				}
			}
		}
	}

	memset(headerBuffer, 0, kHdrSize);
	if (fwrite(headerBuffer, 1, kHdrSize, fd) != (size_t)(kHdrSize)) {
		safs::log_info("{}: fwrite error failed writing header for end marker", __func__);
		return;
	}
}

// Quotas

template <class... Args>
static void fs_store_generic(FILE *fd, Args &&...args) {
	static std::vector<uint8_t> buffer;
	buffer.clear();
	const uint32_t size = serializedSize(std::forward<Args>(args)...);
	serialize(buffer, size, std::forward<Args>(args)...);
	if (fwrite(buffer.data(), 1, buffer.size(), fd) != buffer.size()) {
		safs_pretty_syslog(LOG_NOTICE, "fwrite error");
		return;
	}
}

void MetadataBackendFile::storequotas(FILE *fd) {
	const std::vector<QuotaEntry> &entries =
	    gMetadata->quotaDatabase.getEntries();
	fs_store_generic(fd, entries);
}

// Locks

void MetadataBackendFile::storelocks(FILE *fd) {
	gMetadata->flockLocks.store(fd);
	gMetadata->posixLocks.store(fd);
}

// Full FS

int MetadataBackendFile::process_section(const char *label, uint8_t (&hdr)[kSectionSize],
                                         uint8_t *&ptr, off_t &offbegin,
                                         off_t &offend, FILE *&fd) {
	offend = ftello(fd);
	memcpy(hdr, label, kSectionNameSize);
	ptr = hdr + kSectionNameSize;
	put64bit(&ptr, offend - offbegin - kSectionSize);
	fseeko(fd, offbegin, SEEK_SET);
	if (fwrite(hdr, 1, kSectionSize, fd) != kSectionSize) {
		safs_pretty_syslog(LOG_NOTICE, "fwrite error");
		return SAUNAFS_ERROR_IO;
	}
	offbegin = offend;
	fseeko(fd, offbegin + kSectionSize, SEEK_SET);
	return SAUNAFS_STATUS_OK;
}

void MetadataBackendFile::store(FILE *fd, uint8_t fver) {
	constexpr uint8_t kHeaderSize = sizeof(FilesystemMetadata::maxInodeId) +
	                                sizeof(FilesystemMetadata::metadataVersion) +
	                                sizeof(FilesystemMetadata::nextSessionId);
	uint8_t header[kHeaderSize];

	uint8_t sectionHeader[kSectionSize];
	uint8_t *ptr;
	off_t offbegin{0};
	off_t offend;

	ptr = header;
	putINode(&ptr, gMetadata->maxInodeId);
	put64bit(&ptr, gMetadata->metadataVersion);
	put32bit(&ptr, gMetadata->nextSessionId);

	if (fwrite(header, 1, kHeaderSize, fd) != kHeaderSize) {
		safs_pretty_syslog(LOG_NOTICE, "fwrite error");
		return;
	}

	if (fver >= kMetadataVersionWithSections) {
		offbegin = ftello(fd);
		fseeko(fd, offbegin + kSectionSize, SEEK_SET);
	}

	ptr = sectionHeader;

	storenodes(fd);
	if (fver >= kMetadataVersionWithSections) {
		if (process_section("NODE 1.0", sectionHeader, ptr, offbegin, offend, fd) !=
		    SAUNAFS_STATUS_OK) {
			return;
		}
	}
	storeedges(fd);
	if (fver >= kMetadataVersionWithSections) {
		if (process_section("EDGE 1.0", sectionHeader, ptr, offbegin, offend, fd) !=
		    SAUNAFS_STATUS_OK) {
			return;
		}
	}
	storefree(fd);
	if (fver >= kMetadataVersionWithSections) {
		if (process_section("FREE 1.0", sectionHeader, ptr, offbegin, offend, fd) !=
		    SAUNAFS_STATUS_OK) {
			return;
		}
		xattr_store(fd);
		if (process_section("XATR 1.0", sectionHeader, ptr, offbegin, offend, fd) !=
		    SAUNAFS_STATUS_OK) {
			return;
		}
		fs_store_acls(fd);
		if (process_section("ACLS 1.2", sectionHeader, ptr, offbegin, offend, fd) !=
		    SAUNAFS_STATUS_OK) {
			return;
		}
		storequotas(fd);
		if (process_section("QUOT 1.1", sectionHeader, ptr, offbegin, offend, fd) !=
		    SAUNAFS_STATUS_OK) {
			return;
		}
		storelocks(fd);
		if (process_section("FLCK 1.0", sectionHeader, ptr, offbegin, offend, fd) !=
		    SAUNAFS_STATUS_OK) {
			return;
		}
	}
	chunk_store(fd);
	if (fver >= kMetadataVersionWithSections) {
		if (process_section("CHNK 1.0", sectionHeader, ptr, offbegin, offend, fd) !=
		    SAUNAFS_STATUS_OK) {
			return;
		}

		fseeko(fd, offend, SEEK_SET);
		memcpy(sectionHeader, "[SFS EOF MARKER]", 16);
		if (fwrite(sectionHeader, 1, 16, fd) != (size_t)16) {
			safs_pretty_syslog(LOG_NOTICE, "fwrite error");
			return;
		}
	}
}

void MetadataBackendFile::store_fd(FILE *fd) {
	#if SAUNAFS_VERSHEX >= SAUNAFS_VERSION(2, 9, 0)
	const char hdr[] = SFSSIGNATURE "M 2.9";
	const uint8_t metadataVersion = kMetadataVersionWithLockIds;
#elif SAUNAFS_VERSHEX >= SAUNAFS_VERSION(1, 6, 29)
	const char hdr[] = SFSSIGNATURE "M 2.0";
	const uint8_t metadataVersion = kMetadataVersionWithSections;
#else
	const char hdr[] = SFSSIGNATURE "M 1.6";
	const uint8_t metadataVersion = kMetadataVersionSaunaFS;
#endif

	if (fwrite(&hdr, 1, sizeof(hdr) - 1, fd) != sizeof(hdr) - 1) {
		safs_pretty_syslog(LOG_NOTICE, "fwrite error");
	} else {
		store(fd, metadataVersion);
	}
}

#endif  // #ifndef METALOGGER

void MetadataBackendFile::init() {
	if (fs::exists(kMetadataTmpFilename)) {
		throw MetadataFsConsistencyException(
		    "temporary metadata file (" + std::string(kMetadataTmpFilename) + ") exists,"
		    " metadata directory is in dirty state");
	}

	std::string metadataFile;
	bool metadataFileExists = fs::exists(kMetadataFilename);
	bool legacyMetadataFileExists = fs::exists(kMetadataLegacyFilename);

	if (metadataFileExists) { metadataFile = kMetadataFilename; }

	if (metadataFileExists && legacyMetadataFileExists) {
		metadataFile = kMetadataFilename;
		safs_pretty_syslog(LOG_WARNING,
		                   "There are two metadata files in the data path: %s and %s."
		                   " Please remove the legacy one (%s) to avoid damage to your storage.",
		                   kMetadataFilename, kMetadataLegacyFilename, kMetadataLegacyFilename);
	}

	if (!metadataFileExists && legacyMetadataFileExists) {
		metadataFile = kMetadataLegacyFilename;
		safs_pretty_syslog(
		    LOG_WARNING,
		    "Only Legacy metadata file %s found and will be loaded instead."
		    " You should delete legacy metadata %s on next restart after new metadata %s is created ",
		    metadataFile.c_str(), kMetadataLegacyFilename, kMetadataFilename);
	}

	metadataFile_ = metadataFile;

#if !defined(METALOGGER) && !defined(METARESTORE)
	if (!metadataserver::isMaster() && metadataFile_.empty()) {
		metadataFile_ = kMetadataFilename;
	}

	if (metadataserver::isMaster() && !metadataFileExists && !legacyMetadataFileExists) {
		std::string currentPath = fs::getCurrentWorkingDirectoryNoThrow();
		throw FilesystemException(
		    "can't open metadata file " + currentPath + "/" + kMetadataFilename +
		    ": if this is a new installation create empty metadata by copying " + currentPath +
		    "/" + kMetadataFilename + ".empty to " + currentPath + "/" + kMetadataFilename);
	}
#endif
}

uint64_t MetadataBackendFile::getVersion(const std::string& file) {
	int fd;
	char chkbuff[20];
	char eofmark[16];

	fd = open(file.c_str(), O_RDONLY);
	if (fd<0) {
		throw MetadataCheckException("Can't open the metadata file");
	}

	int bytes = read(fd,chkbuff,20);

	if (bytes < 8) {
		close(fd);
		throw MetadataCheckException("Can't read the metadata file");
	}
	if (memcmp(chkbuff,"SFSM NEW",8)==0) {
		close(fd);
		return 0;
	}
	if (bytes != 20) {
		close(fd);
		throw MetadataCheckException("Can't read the metadata file");
	}

	std::string signature = std::string(chkbuff, 8);
	std::string sfsSignature = std::string(SFSSIGNATURE "M 2.9");
	std::string sauSignature = std::string(SAUSIGNATURE "M 2.9");
	std::string legacySignature = std::string("LIZM 2.9");

	if (signature == sfsSignature || signature == sauSignature) {
		memcpy(eofmark,"[SFS EOF MARKER]",16);
	} else if (signature == legacySignature) {
		safs_pretty_syslog(LOG_WARNING,
		                   "Legacy metadata section header %s, was detected in the metadata file %s",
		                   legacySignature.c_str(), file.c_str());
		memcpy(eofmark,"[MFS EOF MARKER]",16);
	} else {
		close(fd);
		throw MetadataCheckException("Bad EOF MARKER in the metadata file.");
	}

	const uint8_t* ptr = reinterpret_cast<const uint8_t*>(chkbuff + 8 + 4);
	uint64_t version;
	version = get64bit(&ptr);
	lseek(fd,-16,SEEK_END);
	if (read(fd,chkbuff,16)!=16) {
		close(fd);
		throw MetadataCheckException("Can't read the metadata file");
	}
	if (memcmp(chkbuff,eofmark,16)!=0) {
		close(fd);
		throw MetadataCheckException("The metadata file is truncated");
	}
	close(fd);
	return version;
}

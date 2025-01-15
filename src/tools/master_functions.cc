/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2017 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


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

#include "common/platform.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif

#include "common/datapack.h"
#include "common/serialization.h"
#include "common/server_connection.h"	
#include "common/special_inode_defs.h"
#include "common/sockets.h"
#include "common/stat_defs.h"
#include "errors/sfserr.h"
#include "protocol/cltoma.h"
#include "protocol/matocl.h"
#include "tools/tools_common_functions.h"

struct master_info_t {
	uint32_t ip;
	uint16_t port;
	uint32_t cuid;
	uint32_t version;
};

static thread_local int gCurrentMaster = -1;

static int master_register(int rfd, uint32_t cuid) {
	uint32_t i;
	const uint8_t *rptr;
	uint8_t *wptr, regbuff[8 + 73];

	wptr = regbuff;
	put32bit(&wptr, CLTOMA_FUSE_REGISTER);
	put32bit(&wptr, 73);
	memcpy(wptr, FUSE_REGISTER_BLOB_ACL, 64);
	wptr += 64;
	put8bit(&wptr, REGISTER_TOOLS);
	put32bit(&wptr, cuid);
	put16bit(&wptr, SAUNAFS_PACKAGE_VERSION_MAJOR);
	put8bit(&wptr, SAUNAFS_PACKAGE_VERSION_MINOR);
	put8bit(&wptr, SAUNAFS_PACKAGE_VERSION_MICRO);
	if (tcpwrite(rfd, regbuff, 8 + 73) != 8 + 73) {
		printf("register to master: send error\n");
		return -1;
	}
	if (tcpread(rfd, regbuff, 9) != 9) {
		printf("register to master: receive error\n");
		return -1;
	}
	rptr = regbuff;
	i = get32bit(&rptr);
	if (i != MATOCL_FUSE_REGISTER) {
		printf("register to master: wrong answer (type)\n");
		return -1;
	}
	i = get32bit(&rptr);
	if (i != 1) {
		printf("register to master: wrong answer (length)\n");
		return -1;
	}
	if (*rptr) {
		printf("register to master: %s\n", saunafs_error_string(*rptr));
		return -1;
	}
	return 0;
}

static int master_connect(const master_info_t *info) {
	for(int cnt = 0; cnt < 10; ++cnt) {
		int sd = tcpsocket();
		if (sd < 0) {
			return -1;
		}
		int timeout = (cnt % 2) ? (300 * (1 << (cnt >> 1))) : (200 * (1 << (cnt >> 1)));
		if (tcpnumtoconnect(sd, info->ip, info->port, timeout) >= 0) {
			return sd;
		}
		tcpclose(sd);
	}
	return -1;
}

static bool contains_master_info_name_end(const char *name) {
	auto end = name + strlen(name) - 1;
	std::string extracted_name;

	for (; end >= name && *end != '/' && *end != '\\'; --end) {
		extracted_name = *end + extracted_name;
	}

	return (strcmp(extracted_name.c_str(), SPECIAL_FILE_NAME_MASTERINFO) == 0);
}

static int read_master_info(const char *name, master_info_t *info) {
	static constexpr int kMasterInfoSize = 14;
	uint8_t buffer[kMasterInfoSize];
	struct stat stb;
	int sd;

	if (stat(name, &stb) < 0) {
		return -1;
	}

	if ((stb.st_ino != SPECIAL_INODE_MASTERINFO &&
	     !contains_master_info_name_end(name)) ||
	    stb.st_nlink != 1 || stb.st_uid != 0 || stb.st_gid != 0 ||
	    stb.st_size != kMasterInfoSize) {
		return -1;
	}

	sd = open(name, O_RDONLY);
	if (sd < 0) {
		return -2;
	}

	if (read(sd, buffer, kMasterInfoSize) != kMasterInfoSize) {
		close(sd);
		return -2;
	}

	close(sd);

	deserialize(buffer, kMasterInfoSize, info->ip, info->port, info->cuid, info->version);

	return 0;
}

#ifdef _WIN32
int get_inode_by_path(int sd, std::string path, uint32_t &inode) {
	try {
		uint32_t messageId = 0;
		uint32_t rootInodeParent = 1;
		uint32_t parentUid = 0;
		uint32_t parentGid = 0;
		Attributes attr;
		MessageBuffer request;
		cltoma::wholePathLookup::serialize(request, messageId, rootInodeParent,
		                                   path, parentUid, parentGid);
		MessageBuffer response = ServerConnection::sendAndReceive(
		    sd, request, SAU_MATOCL_WHOLE_PATH_LOOKUP);
		PacketVersion packet_version;
		deserializePacketVersionNoHeader(response, packet_version);
		if (packet_version == matocl::wholePathLookup::kStatusPacketVersion) {
			uint8_t status;
			matocl::wholePathLookup::deserialize(response, messageId, status);
			if (status == SAUNAFS_STATUS_OK) {
				fprintf(stderr,
				        "SAU_MATOCL_WHOLE_PATH_LOOKUP: version 0 and "
				        "SAUNAFS_STATUS_OK \n");
				return SAUNAFS_ERROR_IO;
			}
			return status;
		} else if (packet_version ==
		           matocl::wholePathLookup::kResponsePacketVersion) {
			matocl::wholePathLookup::deserialize(response, messageId, inode,
			                                     attr);
			return SAUNAFS_STATUS_OK;
		} else {
			fprintf(stderr,
			        "SAU_MATOCL_WHOLE_PATH_LOOKUP: unknown version: %s \n",
			        std::to_string(packet_version).c_str());
			return SAUNAFS_ERROR_IO;
		}
	} catch (Exception &ex) {
		fprintf(stderr, "SAU_MATOCL_WHOLE_PATH_LOOKUP: %s\n", ex.what());
		return SAUNAFS_ERROR_IO;
	}
}

// Finds the n-th occurrence of a character in a string.
size_t find_nth_occurrence(const std::string &str, char ch, int n) {
	size_t pos = std::string::npos;
	int count = 0;

	for (size_t i = 0; i < str.size(); ++i) {
		if (str[i] == ch) {
			++count;
			if (count == n) {
				pos = i;
				break;
			}
		}
	}

	return pos;
}

// Converts a WSL path (e.g., \\wsl.localhost\Ubuntu-24.04\mnt\c\path) 
// to a Windows path (e.g., C:\path).
void wsl_to_windows_path(char *rpath, size_t rpath_size) {
	std::string wsl_path(rpath);
	std::string windows_path;

	// Find the position of the 5th occurrence of '\\'
	size_t pos_system_folder = find_nth_occurrence(wsl_path, '\\', 4);
	size_t pos_drive_letter = find_nth_occurrence(wsl_path, '\\', 5);

	if (wsl_path[0] == '\\' && wsl_path[1] == '\\' &&
	    pos_system_folder != std::string::npos) {
		if (pos_drive_letter != std::string::npos &&
		    wsl_path.substr(pos_system_folder, 5) == "\\mnt\\") {
			char drive_letter = std::toupper(
			    static_cast<unsigned char>(wsl_path[pos_drive_letter + 1]));

			windows_path += drive_letter;
			windows_path += ":\\";
			windows_path += wsl_path.substr(pos_drive_letter + 3);

			strncpy(rpath, windows_path.c_str(), rpath_size - 1);
			rpath[windows_path.size()] = '\0';
		} else {
			windows_path = wsl_path.substr(pos_system_folder);
			strncpy(rpath, windows_path.c_str(), rpath_size - 1);
			rpath[windows_path.size()] = '\0';
		}
	}
}
#endif

void get_next_path_iteration(std::string &path) {
	size_t pos = std::min(path.find_first_of('\\'), path.find_first_of('/'));
	if (pos != std::string::npos) {
		path = path.substr(pos + 1);
		pos = std::min(path.find_first_of('\\'), path.find_first_of('/'));
		if (pos != std::string::npos) {
			path = path.substr(pos);
		} else {
			path.clear();
		}
	} else {
		path.clear();
	}
}

int open_master_conn(const char *name, uint32_t *inode, mode_t *mode,
                     [[maybe_unused]] bool needrwfs) {
	char rpath[PATH_MAX + 1];
	struct stat stb;
	[[maybe_unused]] struct statvfs stvfsb;
	master_info_t master_info;

	rpath[0] = 0;

	std::string name_to_use = std::string(name);
#ifdef _WIN32
	if (name_to_use.back() == '\\' || name_to_use.back() == '/') {
		name_to_use.pop_back();
	}
#endif
	if (!get_full_path(name_to_use.c_str(), rpath)) {
		printf("%s: get_full_path error\n", name);
		return -1;
	}
#ifndef _WIN32
	if (needrwfs) {
		if (statvfs(rpath, &stvfsb) != 0) {
			printf("%s: (%s) statvfs error: %s\n", name, rpath, strerr(errno));
			return -1;
		}
		if (stvfsb.f_flag & ST_RDONLY) {
			printf("%s: (%s) Read-only file system\n", name, rpath);
			return -1;
		}
	}
#else
	wsl_to_windows_path(rpath, sizeof(rpath));
#endif
	if (stat(rpath, &stb) != 0) {
		printf("%s: (%s) stat error: %s\n", name, rpath, strerr(errno));
		return -1;
	}
	*inode = stb.st_ino;
#ifdef _WIN32
	std::string lookup_rpath;
	if (strlen(rpath) > 3) {
		lookup_rpath = std::string(rpath).substr(2);
		std::replace(lookup_rpath.begin(), lookup_rpath.end(), '\\', '/');
	} else {
		lookup_rpath = "/";
	}
#endif
	if (mode) {
		*mode = stb.st_mode;
	}
	if (gCurrentMaster >= 0) {
		close(gCurrentMaster);
		gCurrentMaster = -1;
	}

	for (;;) {
		uint32_t rpath_inode;

		if (stat(rpath, &stb) != 0) {
			printf("%s: (%s) stat error: %s\n", name, rpath, strerr(errno));
			return -1;
		}
		rpath_inode = stb.st_ino;

		size_t rpath_len = strlen(rpath);
		if (rpath_len + sizeof("/" SPECIAL_FILE_NAME_MASTERINFO) > PATH_MAX) {
			printf("%s: path too long\n", name);
			return -1;
		}

		if (rpath_len == 4 && rpath[2] == '\\' && rpath[3] == '.') {
			strcpy(rpath + rpath_len - 1, SPECIAL_FILE_NAME_MASTERINFO);
		} else if (rpath_len == 3 && rpath[2] == '\\') {
			strcpy(rpath + rpath_len, SPECIAL_FILE_NAME_MASTERINFO);
		} else {
			strcpy(rpath + rpath_len, "/" SPECIAL_FILE_NAME_MASTERINFO);
		}

		int r = read_master_info(rpath, &master_info);
		if (r == -2) {
			printf("%s: can't read '" SPECIAL_FILE_NAME_MASTERINFO "'\n", name);
			return -1;
		}

		if (r == 0) {
			if (master_info.ip == 0 || master_info.port == 0 ||
			    master_info.cuid == 0) {
				printf("%s: incorrect '" SPECIAL_FILE_NAME_MASTERINFO "'\n",
				       name);
				return -1;
			}

			if (rpath_inode == *inode) {
				*inode = SPECIAL_INODE_ROOT;
			}

			int sd = master_connect(&master_info);
			if (sd < 0) {
				printf("%s: can't connect to master (" SPECIAL_FILE_NAME_MASTERINFO "): %s\n", name,
				       strerr(errno));
				return -1;
			}

			if (master_register(sd, master_info.cuid) < 0) {
				printf("%s: can't register to master (" SPECIAL_FILE_NAME_MASTERINFO ")\n", name);
				tcpclose(sd);
				return -1;
			}

#ifdef _WIN32
			if (lookup_rpath == "/") {
				*inode = SPECIAL_INODE_ROOT;
			} else {
				std::string iteration_lookup_rpath = lookup_rpath;
				int inode_by_path_result = SAUNAFS_ERROR_EPERM;
				while (!iteration_lookup_rpath.empty() &&
				       inode_by_path_result != SAUNAFS_STATUS_OK) {
					inode_by_path_result =
					    get_inode_by_path(sd, iteration_lookup_rpath, *inode);
					if (inode_by_path_result != SAUNAFS_STATUS_OK) {
						get_next_path_iteration(iteration_lookup_rpath);
					}
				}
				if (inode_by_path_result != SAUNAFS_STATUS_OK) {
					printf("%s: can't get inode from path: %s\n", name,
					       saunafs_error_string(inode_by_path_result));
					tcpclose(sd);
					return -1;
				}
			}
#endif
			gCurrentMaster = sd;
			return sd;
		}

		// remove .masterinfo from end of string
		rpath[rpath_len] = 0;

#ifdef _WIN32
		if (strlen(rpath) < 3 || !std::isalpha(rpath[0]) || rpath[1] != ':' ||
		    rpath[2] != '\\') {
			printf("%s: not SaunaFS object\n", name);
			return -1;
		}
#else
		if (rpath[0] != '/' || rpath[1] == '\0') {
			printf("%s: not SaunaFS object\n", name);
			return -1;
		}
#endif
		dirname_inplace(rpath);
		if (stat(rpath, &stb) != 0) {
			printf("%s: (%s) stat error: %s\n", name, rpath, strerr(errno));
			return -1;
		}
	}
	return -1;
}

void close_master_conn(int err) {
	if (gCurrentMaster < 0) {
		return;
	}
	if (err) {
		close(gCurrentMaster);
		gCurrentMaster = -1;
	}
}

void force_master_conn_close() {
	if (gCurrentMaster < 0) {
		return;
	}
	close(gCurrentMaster);
	gCurrentMaster = -1;
}

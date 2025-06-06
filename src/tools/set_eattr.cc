/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2016 Skytechnology sp. z o.o.
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

#include <cstring>
#include <stdio.h>
#include <stdlib.h>

#include "common/datapack.h"
#include "errors/saunafs_error_codes.h"
#include "errors/sfserr.h"
#include "tools/tools_commands.h"
#include "tools/tools_common_functions.h"

static void del_eattr_usage() {
	fprintf(stderr,
	        "delete objects extra attributes\n\nusage:\n saunafs deleattr [-nhHr] -f attrname [-f "
	        "attrname ...] "
	        "name [name ...]\n");
	print_numberformat_options();
	print_recursive_option();
	fprintf(stderr, " -f attrname - specify attribute to delete\n");
	print_extra_attributes();
}

static void set_eattr_usage() {
	fprintf(
	    stderr,
	    "set objects extra attributes\n\nusage:\n saunafs seteattr [-nhHr] -f attrname [-f attrname ...] "
	    "name [name ...]\n");
	print_numberformat_options();
	print_recursive_option();
	fprintf(stderr, " -f attrname - specify attribute to set\n");
	print_extra_attributes();
}

static int set_eattr(const char *fname, uint8_t eattr, uint8_t mode) {
	uint32_t cmd, leng, uid;
	inode_t inode, changed, notchanged, notpermitted;
	uint32_t msgid{0};

	int fd;
	fd = open_master_conn(fname, &inode, nullptr, true);
	if (fd < 0) {
		return -1;
	}

	uid = getUId();

	constexpr uint32_t kSetEAttrPayload =
	    sizeof(msgid) + sizeof(inode) + sizeof(uid) + sizeof(eattr) + sizeof(mode);
	constexpr uint32_t kSetEAttrFullSize =
	    sizeof(cmd) + sizeof(kSetEAttrPayload) + kSetEAttrPayload;
	uint8_t reqbuff[kSetEAttrFullSize], *wptr, *buff;
	const uint8_t *rptr;

	wptr = reqbuff;
	put32bit(&wptr, CLTOMA_FUSE_SETEATTR);
	put32bit(&wptr, kSetEAttrPayload);
	put32bit(&wptr, msgid);
	putINode(&wptr, inode);
	put32bit(&wptr, uid);
	put8bit(&wptr, eattr);
	put8bit(&wptr, mode);

	// send request to master
	if (tcpwrite(fd, reqbuff, kSetEAttrFullSize) != kSetEAttrFullSize) {
		printf("%s: master query: send error\n", fname);
		close_master_conn(1);
		return -1;
	}

	// read the first part of the answer
	if (tcpread(fd, reqbuff, sizeof(cmd) + sizeof(leng)) != sizeof(cmd) + sizeof(leng)) {
		printf("%s: master query: receive error\n", fname);
		close_master_conn(1);
		return -1;
	}

	rptr = reqbuff;
	get32bit(&rptr, cmd);
	get32bit(&rptr, leng);

	if (cmd != MATOCL_FUSE_SETEATTR) {
		printf("%s: master query: wrong answer (type)\n", fname);
		close_master_conn(1);
		return -1;
	}

	buff = (uint8_t *)malloc(leng);

	if (tcpread(fd, buff, leng) != (int32_t)leng) {
		printf("%s: master query: receive error\n", fname);
		free(buff);
		close_master_conn(1);
		return -1;
	}

	close_master_conn(0);  // not needed anymore

	rptr = buff;
	get32bit(&rptr, msgid);  // queryid

	if (msgid != 0) {
		printf("%s: master query: wrong answer (queryid)\n", fname);
		free(buff);
		return -1;
	}

	if (leng - sizeof(msgid) == 1) {  // an error code was returned
		printf("%s: %s\n", fname, saunafs_error_string(*rptr));
		free(buff);
		return -1;
	}

	if (leng != sizeof(msgid) + 3 * sizeof(inode_t)) {
		printf("%s: master query: wrong answer (leng)\n", fname);
		free(buff);
		return -1;
	}

	getINode(&rptr, changed);
	getINode(&rptr, notchanged);
	getINode(&rptr, notpermitted);

	if ((mode & SMODE_RMASK) == 0) {
		if (changed) {
			printf("%s: attribute(s) changed\n", fname);
		} else {
			printf("%s: attribute(s) not changed\n", fname);
		}
	} else {
		printf("%s:\n", fname);
		print_number(" inodes with attributes changed:     ", "\n", changed, kMode32, 0, 1);
		print_number(" inodes with attributes not changed: ", "\n", notchanged, kMode32, 0, 1);
		print_number(" inodes with permission denied:      ", "\n", notpermitted, kMode32, 0, 1);
	}

	free(buff);

	return 0;
}

static int gene_eattr_run(int argc, char **argv, uint8_t mode, void (*usage_func)(void)) {
	int i, found, ch, status;
	int rflag = 0;
	uint8_t eattr = 0;

	while ((ch = getopt(argc, argv, "rnhHf:")) != -1) {
		switch (ch) {
		case 'n':
			humode = 0;
			break;
		case 'h':
			humode = 1;
			break;
		case 'H':
			humode = 2;
			break;
		case 'r':
			rflag = 1;
			break;
		case 'f':
			found = 0;
			for (i = 0; found == 0 && i < EATTR_BITS; i++) {
				if (strcmp(optarg, eattrtab[i]) == 0) {
					found = 1;
					eattr |= 1 << i;
				}
			}
			if (!found) {
				fprintf(stderr, "unknown flag\n");
				usage_func();
				return 1;
			}
			break;
		}
	}
	argc -= optind;
	argv += optind;

	if (eattr == 0 && argc >= 1) {
		if (mode == SMODE_INCREASE)
			fprintf(stderr, "no attribute(s) to set\n");
		else
			fprintf(stderr, "no attribute(s) to delete\n");
		usage_func();
		return 1;
	}

	if (argc < 1) {
		usage_func();
		return 1;
	}
	status = 0;
	while (argc > 0) {
		if (set_eattr(*argv, eattr, (rflag) ? (SMODE_RMASK | mode) : mode) < 0) {
			status = 1;
		}
		argc--;
		argv++;
	}
	return status;
}

int set_eattr_run(int argc, char **argv) {
	return gene_eattr_run(argc, argv, SMODE_INCREASE, set_eattr_usage);
}

int del_eattr_run(int argc, char **argv) {
	return gene_eattr_run(argc, argv, SMODE_DECREASE, del_eattr_usage);
}

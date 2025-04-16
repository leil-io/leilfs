/*
 * Copyright 2017 Skytechnology sp. z o.o.
 * Copyright 2023 Leil Storage OÜ
 * Author: Piotr Sarna <sarna@skytechnology.pl>
 *
 * SaunaFS C API Example
 *
 * Compile with -lsaunafs-client and SaunaFS C/C++ library installed.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <saunafs/saunafs_c_api.h>
#include <saunafs/saunafs_error_codes.h>

/* Function that copies saunafs lock interrupt data to provided buffer */
int register_interrupt(sau_lock_interrupt_info_t *info, void *priv) {
	memcpy(priv, info, sizeof(*info));
	return 0;
}

int main(int argc, char **argv) {
	int err;
	sau_err_t sau_err = SAUNAFS_STATUS_OK;
	int i, r;
	sau_t *sau;
	sau_context_t *ctx __attribute__((cleanup(sau_destroy_context)));
	sau_init_params_t params;
	sau_chunkserver_info_t servers[65536];
	struct sau_lock_info lock_info;
	struct sau_fileinfo *fi;
	struct sau_entry entry, entry2;
	struct sau_lock_interrupt_info lock_interrupt_info;
	char buf[1024] = {0};

	/* Create a connection */
	ctx = sau_create_context();
	sau_set_default_init_params(&params, "localhost", (argc > 1) ? argv[1] : "9421", "test123");
	sau = sau_init_with_params(&params);
	if (!sau) {
		fprintf(stderr, "Connection failed\n");
		sau_err = sau_last_err();
		goto destroy_context;
	}
	/* Try to unlink file if it exists and recreate it */
	sau_unlink(sau, ctx, SAUNAFS_INODE_ROOT, "testfile");
	err = sau_mknod(sau, ctx, SAUNAFS_INODE_ROOT, "testfile", 0755, 0, &entry);
	if (err) {
		sau_err = sau_last_err();
		goto destroy_context;
	}
	/* Check if newly created file can be looked up */
	err = sau_lookup(sau, ctx, SAUNAFS_INODE_ROOT, "testfile", &entry2);
	assert(entry.ino == entry2.ino);
	/* Open a file */
	fi = sau_open(sau, ctx, entry.ino, O_RDWR);
	if (!fi) {
		fprintf(stderr, "Open failed\n");
		sau_err = sau_last_err();
		goto destroy_connection;
	}
	/* Write to a file */
	r = sau_write(sau, ctx, fi, 0, 8, "abcdefghijkl");
	if (r < 0) {
		fprintf(stderr, "Write failed\n");
		sau_err = sau_last_err();
		goto release_fileinfo;
	}
	/* Read from a file */
	r = sau_read(sau, ctx, fi, 4, 3, buf);
	if (r < 0) {
		fprintf(stderr, "Read failed\n");
		sau_err = sau_last_err();
		goto release_fileinfo;
	}
	printf("Read %.3s from inode %" PRIiNode "\n", buf, entry.ino);

	/* Get chunkservers info */
	uint32_t reply_size;
	r = sau_get_chunkservers_info(sau, servers, 65536, &reply_size);
	if (r < 0) {
		fprintf(stderr, "Chunkserver info failed\n");
		sau_err = sau_last_err();
		goto release_fileinfo;
	}
	for (i = 0; i < reply_size; ++i) {
		printf("* Chunkserver %u:%u with label %s\n",
	               servers[i].ip, servers[i].port, servers[i].label);
	}
	sau_destroy_chunkservers_info(servers);

	/* Set and get access control lists */
	sau_acl_t *acl = sau_create_acl();
	sau_acl_ace_t acl_ace = {SAU_ACL_ACCESS_ALLOWED_ACE_TYPE, SAU_ACL_SPECIAL_WHO, SAU_ACL_POSIX_MODE_WRITE, 0};
	sau_add_acl_entry(acl, &acl_ace);
	acl_ace.id = 100;
	acl_ace.flags &= ~SAU_ACL_SPECIAL_WHO;
	acl_ace.mask |= SAU_ACL_POSIX_MODE_WRITE;
	sau_add_acl_entry(acl, &acl_ace);
	acl_ace.id = 101;
	acl_ace.flags |= SAU_ACL_IDENTIFIER_GROUP;
	acl_ace.mask &= ~SAU_ACL_APPEND_DATA;
	acl_ace.mask |= SAU_ACL_WRITE_ACL;
	sau_add_acl_entry(acl, &acl_ace);
	size_t acl_reply_size;
	char acl_buf[256] = {};
	r = sau_print_acl(acl, acl_buf, 256, &acl_reply_size);
	if (r < 0) {
		fprintf(stderr, "Printing acl failed\n");
		sau_err = sau_last_err();
		goto release_fileinfo;
	}
	printf("[%d %lu] ACL to set: %s\n", r, acl_reply_size, acl_buf);
	r = sau_setacl(sau, ctx, entry.ino, acl);
	if (r < 0) {
		fprintf(stderr, "setacl failed\n");
		sau_err = sau_last_err();
		goto release_fileinfo;
	}
	sau_destroy_acl(acl);

	memset(acl_buf, 0, 256);
	r = sau_getacl(sau, ctx, entry.ino, &acl);
	if (r < 0) {
		fprintf(stderr, "Getting acl failed\n");
		sau_err = sau_last_err();
		goto release_fileinfo;
	}
	size_t acl_size = sau_get_acl_size(acl);
	printf("ACL size=%lu\n", acl_size);
	for (i = 0; i < acl_size; ++i) {
		sau_get_acl_entry(acl, i, &acl_ace);
		printf("entry %u %u %x\n", acl_ace.id, acl_ace.type, acl_ace.mask);
	}
	r = sau_print_acl(acl, acl_buf, 256, &acl_reply_size);
	if (r < 0) {
		fprintf(stderr, "Printing acl failed\n");
		sau_err = sau_last_err();
		goto release_fileinfo;
	}
	printf("[%d %lu] ACL extracted: %s\n", r, acl_reply_size, acl_buf);
	sau_destroy_acl(acl);

	lock_info.l_type = F_WRLCK;
	lock_info.l_start = 0;
	lock_info.l_len = 3;
	lock_info.l_pid = 19;

	r = sau_setlk(sau, ctx, fi, &lock_info, NULL, NULL);
	if (r < 0) {
		fprintf(stderr, "Setlk failed\n");
		sau_err = sau_last_err();
		goto release_fileinfo;
	}
	printf("Lock info 1: %d %ld %ld %d\n", lock_info.l_type, lock_info.l_start, lock_info.l_len, lock_info.l_pid);

	r = sau_getlk(sau, ctx, fi, &lock_info);
	if (r < 0) {
		fprintf(stderr, "Getlk failed\n");
		sau_err = sau_last_err();
		goto release_fileinfo;
	}
	printf("Lock info 2: %d %ld %ld %d\n", lock_info.l_type, lock_info.l_start, lock_info.l_len, lock_info.l_pid);

	lock_info.l_type = F_UNLCK;
	lock_info.l_len = 1;
	r = sau_setlk(sau, ctx, fi, &lock_info, NULL, NULL);
	if (r < 0) {
		fprintf(stderr, "Setlk failed\n");
		sau_err = sau_last_err();
		goto release_fileinfo;
	}
	lock_info.l_type = F_WRLCK;
	lock_info.l_len = 2;
	r = sau_getlk(sau, ctx, fi, &lock_info);
	if (r < 0) {
		fprintf(stderr, "Getlk2 failed\n");
		sau_err = sau_last_err();
		goto release_fileinfo;
	}
	printf("Lock info 3: %d %ld %ld %d\n", lock_info.l_type, lock_info.l_start, lock_info.l_len, lock_info.l_pid);

	lock_info.l_type = F_WRLCK;
	lock_info.l_len = 3;
	r = sau_setlk(sau, ctx, fi, &lock_info, &register_interrupt, &lock_interrupt_info);
	if (r < 0) {
		fprintf(stderr, "Setlk failed\n");
		sau_err = sau_last_err();
		goto release_fileinfo;
	}
	printf("Filled interrupt info: %lx %" PRIiNode " %u\n", lock_interrupt_info.owner,
	       lock_interrupt_info.ino, lock_interrupt_info.reqid);

release_fileinfo:
	sau_release(sau, fi);
destroy_connection:
	sau_destroy(sau);
destroy_context:

	printf("Program status: %s\n", sau_error_string(sau_err));
	return sau_error_conv(sau_err);
}

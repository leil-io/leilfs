timeout_set 3 minutes
assert_program_installed setfacl getfacl

# Regression test for shadow-master Access Control List (ACL) synchronization.
#
# Forces a shadow master to rebuild ACLs from the saved metadata image and then converge through
# changelog replay, and proves the ACL state survives a shadow promotion unchanged.
#
# ACLs are stored separately from node serialization and are NOT part of the metadata checksum, so
# a checksum match alone would not surface an ACL gap -- but metadata_print captures them via
# getfacl, so assert_no_diff after promotion does.
#
# The main probe is ACLs set BEFORE the dump and never touched afterwards: they exist only in the
# dumped image (the post-dump changelog carries no SETACL for them), so a backend that does not
# persist ACLs loses them on the shadow. A small post-dump churn is also included to exercise the
# ACL replay path.

# METADATA_DUMP_PERIOD_SECONDS = 0 disables periodic dumping so the master does not auto-re-dump a
# fresh image; the shadow must rebuild the ACLs from the saved metadata image.
master_cfg="METADATA_DUMP_PERIOD_SECONDS = 0"

CHUNKSERVERS=3 \
	MASTERSERVERS=2 \
	MOUNTS=2 \
	USE_RAMDISK="YES" \
	MOUNT_0_EXTRA_CONFIG="sfscachemode=NEVER,sfsreportreservedperiod=1,sfsdirentrycacheto=0" \
	MOUNT_1_EXTRA_CONFIG="sfsmeta" \
	SFSEXPORTS_EXTRA_OPTIONS="allcanchangequota,ignoregid" \
	SFSEXPORTS_META_EXTRA_OPTIONS="nonrootmeta" \
	MASTER_EXTRA_CONFIG="$master_cfg" \
	setup_local_empty_saunafs info

export SFS_META_MOUNT_PATH=${info[mount1]}
export CHANGELOG="${info[master_data_path]}"/changelog.sfs

# Generate namespace operations.
cd "${info[mount0]}"
metadata_generate_files
metadata_generate_funny_inodes
metadata_generate_chunks

# ACLs set BEFORE the dump and NOT touched afterwards. Only the dumped image carries them, so a
# backend that does not persist ACLs loses them on the shadow.
mkdir acl_static
touch acl_static/file{1..40}
for i in {1..40}; do
	setfacl -m user:saunafstest:rwx -m group:fuse:r-x acl_static/file$i
done
mkdir acl_static_dirs
for i in {1..10}; do
	mkdir acl_static_dirs/dir$i
	setfacl -d -m group:fuse:rwx -m user:saunafstest:rw- acl_static_dirs/dir$i
done

# ACLs on files that WILL be churned after the dump (to exercise the replay path).
mkdir acl_churn
touch acl_churn/file{1..20}
for i in {1..20}; do
	setfacl -m user:saunafstest:rwx acl_churn/file$i
done
cd

# Dump the metadata image: the ACLs above exist at this point.
assert_success saunafs_admin_master save-metadata

# Churn ACLs AFTER the dump on the acl_churn files (the acl_static ones are intentionally left
# untouched so they probe persistence rather than replay).
cd "${info[mount0]}"
for i in {1..20}; do
	setfacl -m user:saunafstest:r-- acl_churn/file$i
done
cd

# Start the shadow AFTER the dump so it must rebuild ACLs: load the saved metadata image, then
# converge through changelog replay.
saunafs_master_n 1 start
assert_eventually "saunafs_shadow_synchronized 1"

# Capture the namespace (including ACLs via getfacl) as served by the original master.
cd "${info[mount0]}"
metadata=$(metadata_print)
cd

# Simulate master failure and recover from the shadow.
saunafs_master_daemon kill
saunafs_make_conf_for_master 1
saunafs_master_daemon reload
saunafs_wait_for_all_ready_chunkservers

# The promoted shadow must serve identical ACLs.
cd "${info[mount0]}"
assert_no_diff "$metadata" "$(metadata_print)"
metadata_validate_files

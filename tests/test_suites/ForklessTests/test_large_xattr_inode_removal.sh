timeout_set 5 minutes
assert_program_installed setfattr
assert_program_installed fdbcli

# The name-list limit does not bound the total value bytes of an inode's xattrs. Deleting a file
# with enough large attributes must not turn its checkpoint undo capture into one oversized FDB
# transaction that the metadata writer can never flush.
CHUNKSERVERS=1 \
	METADATA_BACKEND=FORKLESS \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER,sfsreportreservedperiod=1" \
	setup_local_empty_saunafs info

live_xattr_count() {
	fdbcli --exec 'getrangekeys XATR_ XATS_ 500' \
		--cluster-file "${workspace}/conf/fdb.cluster" |
		awk '/^`XATR_/ { count++ } END { print count+0 }'
}

undo_xattr_count() {
	fdbcli --exec 'getrangekeys XATRU_ XATRV_ 500' \
		--cluster-file "${workspace}/conf/fdb.cluster" |
		awk '/^`XATRU_/ { count++ } END { print count+0 }'
}

cd "${info[mount0]}"
touch large_xattrs
# Bypass trash; the mount reports released sessions before reserved inodes are finally deleted.
saunafs settrashtime 0 large_xattrs
for index in $(seq -w 1 160); do
	# Prefix 63,996 base64 bytes with text so setfattr never mistakes a random 0x/0s prefix
	# for encoded input. Distinct 64,000-byte values exceed FDB's limit in aggregate.
	xattr_value="data$(head -c 47997 /dev/urandom | base64 -w0)"
	setfattr -n "user.bulk_${index}" -v "$xattr_value" large_xattrs
done

# Make the large pre-image durable before deleting the inode. The next save must drain
# the removal and its undo records; one inode-wide range event exceeds FDB's limit.
assert_success saunafs_admin_master save-metadata
assert_equals 160 "$(live_xattr_count)"
rm large_xattrs
# Wait for the mount's session release and for every ordered removal event to flush.
# The old range event cannot commit and leaves all 160 live keys in FDB.
assert_eventually_prints 0 live_xattr_count '60 seconds'
assert_success saunafs_admin_master save-metadata
assert_equals 0 "$(live_xattr_count)"
assert_equals 160 "$(undo_xattr_count)"

saunafs_master_daemon restart
assert_failure test -e large_xattrs

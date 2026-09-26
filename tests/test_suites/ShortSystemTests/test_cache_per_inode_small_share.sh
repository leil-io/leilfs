# A write far larger than an inode's share of the write cache completes, as the
# inode's cached data is written out, and reads back intact.
timeout_set 1 minute

# 1% of a 16 MiB write cache lets an inode hold only a couple of blocks
USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER|sfswritecachesize=16|sfscacheperinodepercentage=1" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

# The writer records its exit status once it finishes
writer_status="${TEMP_DIR}/writer_status"
{
	status=0
	FILE_SIZE=8M file-generate file || status=$?
	echo ${status} > "${writer_status}"
} &
assert_eventually "test -s '${writer_status}'" '30 seconds'
assert_equals 0 "$(cat "${writer_status}")"
assert_success file-validate file

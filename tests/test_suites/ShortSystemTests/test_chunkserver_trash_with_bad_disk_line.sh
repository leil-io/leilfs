timeout_set 4 minutes

# An invalid line in the disk list must not disable the chunk trash.
CHUNKSERVERS=1 \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	MASTER_EXTRA_CONFIG="CHUNKS_LOOP_MIN_TIME = 1`
			`|CHUNKS_LOOP_MAX_CPU = 90`
			`|OPERATIONS_DELAY_INIT = 1" \
	CHUNKSERVER_EXTRA_CONFIG="CHUNK_TRASH_ENABLED=1`
			`|CHUNK_TRASH_EXPIRATION_SECONDS=1000`
			`|CHUNK_TRASH_FREE_SPACE_THRESHOLD_GB=0" \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

# Start again with a line the disk list's parser refuses after the disks.
assert_success saunafs_chunkserver_daemon 0 stop
echo "*" >> "${info[chunkserver0_hdd]}"
assert_success saunafs_chunkserver_daemon 0 start
saunafs_wait_for_all_ready_chunkservers
# Drop the line again, without a reload, for the helpers that read the list.
sed -i '$d' "${info[chunkserver0_hdd]}"

file="${info[mount0]}/file"
touch "${file}"
saunafs setgoal 1 "${file}"
dd if=/dev/zero of="${file}" bs=1MiB count=2
saunafs settrashtime 0 "${file}"
assert_success test "$(find_all_chunks | wc -l)" -ge 1

rm -f "${file}"
assert_eventually_prints 0 "find_all_chunks | wc -l" "2 minutes"
assert_success test "$(find_all_trashed_chunks | wc -l)" -ge 1

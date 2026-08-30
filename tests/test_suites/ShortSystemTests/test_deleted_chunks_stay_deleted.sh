timeout_set 4 minutes

# CHUNKS_LOOP_MIN_TIME/CHUNKS_SOFT_DEL_LIMIT: let the chunk loop drop unused chunks quickly.
# OPERATIONS_DELAY_INIT: the loop must already run in the first phase of the test.
# METADATA_DUMP_PERIOD_SECONDS: no periodic dumps, so the metadata store is only written by the
# regular mutation path and by the explicit save below.
master_cfg="METADATA_DUMP_PERIOD_SECONDS = 0"
master_cfg+="|CHUNKS_LOOP_MIN_TIME = 1"
master_cfg+="|CHUNKS_LOOP_MAX_CPU = 90"
master_cfg+="|CHUNKS_SOFT_DEL_LIMIT = 100"
master_cfg+="|OPERATIONS_DELAY_INIT = 1"
master_cfg+="|OPERATIONS_DELAY_DISCONNECT = 1"

CHUNKSERVERS=2 \
	MOUNTS=1 \
	USE_RAMDISK="YES" \
	MOUNT_0_EXTRA_CONFIG="sfscachemode=NEVER" \
	MASTER_EXTRA_CONFIG="${master_cfg}" \
	setup_local_empty_saunafs info

# Number of chunks the master currently keeps in memory.
master_chunk_count() {
	saunafs_admin_master info | awk -F'\t' '/^Chunks:/ {print $2}'
}

cd "${info[mount0]}"
mkdir chunk_removal
# A trashed file still references its chunks, so keep deletions out of the trash.
saunafs settrashtime 0 chunk_removal
cd chunk_removal

file_count=5
for i in $(seq "${file_count}"); do
	FILE_SIZE=1K file-generate "file_${i}"
done
assert_eventually_prints "${file_count}" 'master_chunk_count'

rm -f file_*

# The chunk loop drops a chunk once no file references it and every copy is gone.
assert_eventually_prints 0 'master_chunk_count' '2 minutes'

# Keep the chunk loop from running after the restart: a chunk resurrected by the metadata load
# must stay visible instead of being deleted again before the assertion below reads it.
cd
sed -i 's/^OPERATIONS_DELAY_INIT.*/OPERATIONS_DELAY_INIT = 3600/' "${info[master_cfg]}"

assert_success saunafs_admin_master save-metadata
assert_success saunafs_master_daemon restart
saunafs_wait_for_all_ready_chunkservers

# Deleted chunks must not come back from the metadata store. Backends that persist chunks
# individually have to remove the chunk's row when the chunk is deleted; without that, every
# restart reloads the deleted chunks as zombies and the count below is non-zero.
# The chunk loop is disabled here, so a resurrected chunk cannot be cleaned up before this reads
# it, and the chunkservers have already reported the chunks they hold.
assert_equals 0 "$(master_chunk_count)"

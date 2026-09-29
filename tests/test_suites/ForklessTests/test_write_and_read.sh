timeout_set 2 minutes

# Write and read back data under the forkless (FoundationDB) metadata backend.
#
# Covers the standard, xor and erasure-coding replication goals, validates the mount's read and
# write statistics, and then restarts the master. The restarted master has no metadata image to
# read: it rebuilds its chunk metadata from the live FDB keyspace and the sealed checkpoint, so
# revalidating every file afterwards proves the chunk rows written during the test were persisted
# and reloaded correctly.

WRITE_CACHE_SIZE_mb=128
ONE_TO_NINE=123456789

CHUNKSERVERS=5 \
	METADATA_BACKEND="FORKLESS" \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER,sfswritecachesize=${WRITE_CACHE_SIZE_mb}" \
	CHUNKSERVER_EXTRA_CONFIG="READ_AHEAD_KB = 1024|MAX_READ_BEHIND_KB = 2048" \
	setup_local_empty_saunafs info

cd ${info[mount0]}

mkdir dir_std
FILE_SIZE="${ONE_TO_NINE}" BLOCK_SIZE=12345 file-generate dir_std/file
if ! file-validate dir_std/file; then
	test_add_failure "Data read from file is different than written"
fi

mkdir dir_xor
saunafs setgoal -r xor2 dir_xor
FILE_SIZE="${ONE_TO_NINE}" BLOCK_SIZE=12345 file-generate dir_xor/file
if ! file-validate dir_xor/file; then
	test_add_failure "Data read from file is different than written"
fi

mkdir dir_ec
saunafs setgoal -r ec32 dir_ec
FILE_SIZE="${ONE_TO_NINE}" BLOCK_SIZE=12345 file-generate dir_ec/file
if ! file-validate dir_ec/file; then
	test_add_failure "Data read from file is different than written"
fi

# Checks some stats
cat .stats > "${TEMP_DIR}/stats_results"

read_from_new_req=$(grep 'read_details.read_from_new_req' "${TEMP_DIR}/stats_results" \
	| awk '{print $2}')
assert_less_than 3 "${read_from_new_req}"
free_cache_avg_kb=$(grep 'write_details.free_cache_avg_kb' "${TEMP_DIR}/stats_results" \
	| awk '{print $2}')
assert_less_than "${free_cache_avg_kb}" $((WRITE_CACHE_SIZE_mb * 1024 + 1))

# Check resetting stats
echo "Something to reset the stats" | sudo tee .stats > /dev/null
cat .stats > "${TEMP_DIR}/stats_results"

req_read_bytes=$(grep 'read_details.req_read_bytes' "${TEMP_DIR}/stats_results" | awk '{print $2}')
assert_equals "${req_read_bytes}" 0
cached_bytes=$(grep 'write_details.cached_bytes' "${TEMP_DIR}/stats_results" | awk '{print $2}')
assert_equals "${cached_bytes}" 0

# Restart the master: it reloads chunk metadata from FDB instead of a metadata image.
saunafs_master_daemon restart
saunafs_wait_for_all_ready_chunkservers

# Validate the files written before restarting the master
assert_success file-validate dir_std/file
assert_success file-validate dir_xor/file
assert_success file-validate dir_ec/file

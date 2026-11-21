timeout_set '1 minute'

# With two EC(2,1) servers stopped, one known part cannot reconstruct a chunk.
# Reads and in-place writes must fail within a bounded time rather than wait
# indefinitely for registration that is not running. Restarting the servers
# must restore access and preserve data.

FILE_COUNT=${FILE_COUNT:-10}
FILE_SIZE=${FILE_SIZE:-262144}
# Allow client retries and query timeout while detecting indefinite waits.
MAX_FAIL_SECONDS=60

CHUNKSERVERS=3 \
	DISK_PER_CHUNKSERVER=1 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
	MASTER_CUSTOM_GOALS="10 ec21: \$ec(2,1)" \
	CHUNKSERVER_EXTRA_CONFIG="MASTER_RECONNECTION_DELAY = 1" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER,sfsioretries=3" \
	setup_local_empty_saunafs info

file_path() {
	echo "${info[mount0]}/ec21/$(printf 'file_%05d' "$1")"
}

ec_dir="${info[mount0]}/ec21"
mkdir "$ec_dir"
assert_success saunafs setgoal ec21 "$ec_dir"

for ((i = 0; i < FILE_COUNT; ++i)); do
	FILE_SIZE=$FILE_SIZE file-generate "$(file_path "$i")"
done
probe_file=$(file_path 0)
MESSAGE="every chunk must start with all 3 parts known" \
	assert_eventually_equals "echo 3" "count_registered_parts '$probe_file'"
echo "PHASE: $FILE_COUNT EC(2,1) chunks created, 3 parts each"

# Remount to clear client caches before probing the master.
saunafs_mount_unmount 0
saunafs_mount_start 0

# Leave one known EC part, with no registration supplying the missing parts.
for csid in 1 2; do
	saunafs_chunkserver_daemon "$csid" stop
done
MESSAGE="the chunk must be left below the EC(2,1) threshold, permanently" \
	assert_eventually_equals "echo 1" "count_registered_parts '$probe_file'"
echo "PHASE: chunkservers 1 and 2 stopped, chunk down to 1 of 3 parts for good"

# Read the whole file: a short range may be served by the surviving EC data
# part, depending on placement, and would not prove the chunk is recoverable.
read_probe() {
	dd if="$probe_file" of=/dev/null bs=1M status=none
}
read_start_ns=$(date +%s%N)
MESSAGE="read of a permanently incomplete chunk must fail, not hang" \
	assert_failure read_probe
read_ms=$(( ($(date +%s%N) - read_start_ns) / 1000000 ))

# Preserve the existing chunk and flush the write so failures reach dd.
write_probe() {
	dd if=/dev/zero of="$probe_file" bs=4096 count=1 conv=notrunc,fsync status=none
}
write_start_ns=$(date +%s%N)
MESSAGE="write to a permanently incomplete chunk must fail, not hang" \
	assert_failure write_probe
write_ms=$(( ($(date +%s%N) - write_start_ns) / 1000000 ))

echo "LOST_CHUNK_READ_FAIL_MS: $read_ms"
echo "LOST_CHUNK_WRITE_FAIL_MS: $write_ms"

assert_less_than "$read_ms" "$((MAX_FAIL_SECONDS * 1000))"
assert_less_than "$write_ms" "$((MAX_FAIL_SECONDS * 1000))"

# Restore all parts and verify access to the same data.
for csid in 1 2; do
	saunafs_chunkserver_daemon "$csid" start
done
saunafs_wait_for_all_ready_chunkservers
MESSAGE="the chunk must be complete again once the chunkservers return" \
	assert_eventually_equals "echo 3" "count_registered_parts '$probe_file'"

# Validate original contents before overwriting the recovered chunk.
MESSAGE="read of the recovered chunk must succeed and return the original data" \
	assert_success file-validate "$probe_file"
MESSAGE="write to the recovered chunk must succeed" \
	assert_success write_probe
MESSAGE="the recovered chunk must read back what was just written" \
	assert_success cmp -n 4096 "$probe_file" /dev/zero

# Check an untouched file for data loss.
assert_success file-validate "$(file_path 1)"

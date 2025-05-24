timeout_set '1 minutes'
period=10

CHUNKSERVERS=1 \
	MASTERSERVERS=1 \
	MASTER_EXTRA_CONFIG="METADATA_DUMP_PERIOD_SECONDS = 10|MAGIC_DEBUG_LOG = ${TEMP_DIR}/log" \
	setup_local_empty_saunafs info

is_time_for_metadata_dump() {
	# The period is 10 seconds, so the modulo should be 0 every 10 seconds in UNIX time
	[ "$(( $(date +%s) % period))" -eq 0 ]
}

echo "metadata dump will take place every ${period} seconds..."

# Create a lot of small files to generate chunks
mkdir ${info[mount0]}/1M.files
for i in $(seq 1 2000); do
	dd if=/dev/zero of="${info[mount0]}/1M.files/file_${i}" bs=1M count=1 status=none
done

# Stop the chunkserver daemon
saunafs_chunkserver_daemon 0 stop

# Clean the logs
truncate -s0 "${TEMP_DIR}/log"

# Start the chunkserver again to trigger chunks registration
saunafs_chunkserver_daemon 0 start

saunafs_wait_for_ready_chunkservers 1

# Wait for the next metadata dump
while ! is_time_for_metadata_dump; do
	echo "Waiting for next metadata dump, current seconds: $(date +%S)"
	sleep 1
done

# Confirm the metadata dump is skipped during chunks registration in the master
assert_eventually "grep -q 'metadata dump was skipped' ${TEMP_DIR}/log"

# Confirm the metadata dump is successful in the shadow
assert_eventually "grep -q 'periodic metadata dump: success' ${TEMP_DIR}/log"

# Clean the logs
truncate -s0 "${TEMP_DIR}/log"

# Wait for the next metadata dump
while ! is_time_for_metadata_dump; do
	echo "Waiting for next metadata dump, current seconds: $(date +%S)"
	sleep 1
done

# Confirm metadata dump was not skipped in the master
assert_awk_finds_no '/metadata dump was skipped:/' "${TEMP_DIR}/log"

# Confirm metadata dump was successful in the master and shadow
assert_eventually "grep 'periodic metadata dump: success' ${TEMP_DIR}/log | wc -l" "2"

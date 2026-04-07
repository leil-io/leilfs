timeout_set '2 minutes'
period=10
wait_period=3
master_cfg="METADATA_DUMP_PERIOD_SECONDS = ${period}"
master_cfg+="|MAGIC_DEBUG_LOG = ${TEMP_DIR}/log|LOG_FLUSH_ON=DEBUG"

CHUNKSERVERS=1 \
	MASTERSERVERS=1 \
	MASTER_EXTRA_CONFIG="${master_cfg}" \
	setup_local_empty_saunafs info

is_time_for_metadata_dump() {
	# The period is 10 seconds, so the modulo should be 0 every 10 seconds in UNIX time
	[ "$(( $(date +%s) % period))" -eq 0 ]
}

truncate_log() {
	truncate -s0 "${TEMP_DIR}/log"
}

# Function to check if number of metadata dumps is even
# Returns 0 for even number, 1 otherwise
is_even_metadata_dump_count() {
	local dumps=$1
	local result=$(( dumps % 2 == 0 ? 0 : 1 ))
	echo "$result"
}

echo "metadata dump will take place every ${period} seconds..."

# Create a lot of small files to generate chunks
mkdir ${info[mount0]}/1M.files
for i in $(seq 1 2000); do
	dd if=/dev/zero of="${info[mount0]}/1M.files/file_${i}" bs=1M count=1 status=none
done

# Stop the chunkserver daemon
saunafs_chunkserver_daemon 0 stop

# Sync to a dump boundary before restarting the chunkserver, so that registration happens
# right at the start of a new dump period. This gives us a full period (~10s) before the next
# dump fires, which is safely within the 15-second chunks registration timeout.
echo "Syncing with dump period before restarting the chunkserver..."
while ! is_time_for_metadata_dump; do
	sleep 0.2
done
sleep 1  # let the boundary pass

# Start the chunkserver again to trigger chunks registration
saunafs_chunkserver_daemon 0 start

saunafs_wait_for_ready_chunkservers 1

# Wait for the next metadata dump execution
truncate_log
while ! is_time_for_metadata_dump; do
	echo "Waiting for next metadata dump, current seconds: $(date +%S)"
	sleep 1
done

# It's time for the metadata dump, wait a bit more for the master to finish it
echo "Time for metadata dump, waiting ${wait_period} seconds, current seconds: $(date +%S)"
sleep ${wait_period}

# Confirm the metadata dump is skipped during chunks registration in the master
log=$(cat "${TEMP_DIR}/log")
truncate_log
assert_awk_finds '/periodic metadata dump was skipped/' "${log}"

# Confirm the metadata dump is successful in the shadow
assert_awk_finds '/periodic metadata dump:/' "${log}"

# Wait for the next metadata dump
truncate_log
while ! is_time_for_metadata_dump; do
	echo "Waiting for next metadata dump, current seconds: $(date +%S)"
	sleep 1
done

# It's time for the next metadata dump, wait a bit more for the master to finish it
echo "Time for next metadata dump, waiting ${wait_period} seconds, current seconds: $(date +%S)"
sleep ${wait_period}

# Wait for the last metadata dump
truncate_log
while ! is_time_for_metadata_dump; do
	echo "Waiting for last metadata dump, current seconds: $(date +%S)"
	sleep 1
done

# It's time for the last metadata dump, wait a bit more for the master to finish it
echo "Time for last metadata dump, waiting ${wait_period} seconds, current seconds: $(date +%S)"
sleep ${wait_period}

# Confirm metadata dump was not skipped in the master
log=$(cat "${TEMP_DIR}/log")
truncate_log
assert_awk_finds_no '/metadata dump was skipped/' "${log}"

dumps_done=$(grep -c 'periodic metadata dump:' <<< "${log}")

# Expect at least one successful metadata dump in the shadow
assert_less_or_equal "1" "${dumps_done}"

# Confirm metadata dumps were successful in the master and shadow
# The validation is relaxed to allow for an even number of dumps in case the test runs
# using lower resources, like Docker or virtual machines
assert_equals 0 $(is_even_metadata_dump_count "${dumps_done}")

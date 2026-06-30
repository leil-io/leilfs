timeout_set 90 seconds

master_log_file="${TEMP_DIR}/log"

master_cfg="PERSONALITY=ha-cluster-managed"
master_cfg+="|MAGIC_DEBUG_LOG = ${master_log_file}|LOG_FLUSH_ON=DEBUG"

CHUNKSERVERS=3 \
	USE_RAMDISK="YES" \
	MASTER_EXTRA_CONFIG="${master_cfg}" \
	MASTER_START_PARAM="-o ha-cluster-managed -o initial-personality=master" \
	SHADOW_START_PARAM="-o ha-cluster-managed -o initial-personality=shadow" \
	setup_local_empty_saunafs info

saunafs_master_daemon_ha () {
	local command=$1
	shift
	saunafs_master_daemon ${command} -o ha-cluster-managed $@
}

# Function to get birth time with nanosecond precision
get_birth_time_timestamp() {
	local file_path="$1"
	local stat_info=$(stat "${file_path}")
	# Generate birth timestamp with the format "HH:MM:SS.nnnnnnnnn"
	local birth_time=$(echo "${stat_info}" | grep 'Birth' | awk '{print $3}')
	# Format the birth timestamp to an integer with the format "HHMMSSnnnnnnnnn"
	local timestamp=$(echo "${birth_time}" | sed 's/[^0-9]*//g')
	echo "${timestamp}"
}

cd "${info[mount0]}"

# Create some test data to ensure metadata changes exist
mkdir test_dir
saunafs setgoal 2 test_dir
echo "test_data" > test_dir/test_file.txt

echo "Starting test with deferred metadata dump..."

echo "Stopping primary master to simulate failover..."
assert_success saunafs_master_daemon_ha stop

# Verify that starting sfsmaster with default values does not trigger a deferred metadata dump
log=$(cat "${master_log_file}")
assert_awk_finds_no '/Executing deferred metadata dump/' "${log}"

# Message for the usual metadata dump
assert_awk_finds '/Metadata dumped successfully after applying changelogs/' "${log}"

# Save birth timestamp of metadata.sfs after stopping the initial master
birth_timestamp_after_stop=$(get_birth_time_timestamp "${info[master_data_path]}/metadata.sfs")

# Restart the master server, this time using `-o defer-metadata-dump` option
assert_success saunafs_master_daemon_ha start -o initial-personality=master \
	-o auto-recovery -o defer-metadata-dump

# After restarting the master, the client and chunkservers reconnect asynchronously. A stat
# can be served from the kernel attribute cache before the client<->master session is back,
# so gate on the write itself: retry it until the reconnect completes, otherwise it could
# fail with "Transport endpoint is not connected".
saunafs_wait_for_all_ready_chunkservers

echo "Testing filesystem functionality during metadata dump..."
assert_eventually 'echo "additional_data_during_dump" > test_dir/during_dump_file.txt' "60 seconds"

assert_equals "test_data" "$(cat test_dir/test_file.txt)"
assert_equals "additional_data_during_dump" "$(cat test_dir/during_dump_file.txt)"

echo "Verifying deferred metadata dump was scheduled..."
log=$(cat "${master_log_file}")
assert_awk_finds '/Deferring metadata dump option detected/' "${log}"

echo "Waiting for deferred metadata dump to execute..."
log=$(cat "${master_log_file}")
assert_awk_finds '/Executing deferred metadata dump/' "${log}"

echo "Testing filesystem remains functional during background metadata dump..."

echo "data_during_background_dump" > test_dir/background_dump_file.txt
assert_equals "additional_data_during_dump" "$(cat test_dir/during_dump_file.txt)"
assert_equals "data_during_background_dump" "$(cat test_dir/background_dump_file.txt)"

echo "Waiting for deferred metadata dump to complete..."
log=$(cat "${master_log_file}")
assert_awk_finds '/Deferred metadata dump completed successfully/' "${log}"

# Save birth timestamp of metadata.sfs after starting the master
birth_timestamp_after_start=$(get_birth_time_timestamp "${info[master_data_path]}/metadata.sfs")

# Validate the file was actually updated
assert_less_than "${birth_timestamp_after_stop}" "${birth_timestamp_after_start}"
echo "Verified: metadata.sfs was updated by deferred dump"
echo "  Birthtime after stop: ${birth_timestamp_after_stop}"
echo "  Birthtime after start:  ${birth_timestamp_after_start}"
echo "  Difference: $((birth_timestamp_after_start - birth_timestamp_after_stop))"

echo "Verifying data integrity after deferred dump completion..."

assert_equals "test_data" "$(cat test_dir/test_file.txt)"
assert_equals "additional_data_during_dump" "$(cat test_dir/during_dump_file.txt)"
assert_equals "data_during_background_dump" "$(cat test_dir/background_dump_file.txt)"

echo "Verifying no metadata corruption by checking filesystem consistency..."
saunafs-admin metadataserver-status localhost "${info[matocl]}"

echo "Checking for error messages in master logs..."
log=$(cat "${master_log_file}")
assert_awk_finds_no '/Deferred metadata dump failed/' "${log}"

mkdir test_dir/final_test
echo "final_test_data" > test_dir/final_test/final_file.txt
assert_equals "final_test_data" "$(cat test_dir/final_test/final_file.txt)"

echo "Test handling deferred metadata dump completed successfully!"

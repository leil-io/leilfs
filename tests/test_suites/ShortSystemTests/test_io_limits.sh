timeout_set 2 minutes

# Create a config file with a limit of 1 MB/s for all processes
iolimits="$TEMP_DIR/iolimits.cfg"
echo "limit unclassified 1024" > "$iolimits"

# Create a second config file with a limit of 2 MB/s for all processes
iolimits2="$TEMP_DIR/iolimits2.cfg"
echo "limit unclassified 2048" > "$iolimits2"

# Check the wall-clock time of a rate-limited operation. The slow tolerance is later
# scaled by the machine multiplier to tolerate heavy workloads; the fast side stays strict.
assert_limited_io_time() {
	local expected_time_ms=$1
	local actual_time_ms=$2
	local slow_tolerance_ms=$(( (250 + expected_time_ms * 15 / 100) * $(timeout_get_total_multiplier) ))
	assert_less_or_equal $((expected_time_ms - 250)) "${actual_time_ms}"
	assert_less_or_equal "${actual_time_ms}" $((expected_time_ms + slow_tolerance_ms))
}

run_io_test() {
	local limit_mbps=$1
	local expected_time_divisor=$2

	time=$(which time) # We need /usr/bin/time or something like this in this test, not a bash built-in
	head -c 1M /dev/zero > warmup

	for mb in 9 5 3 1; do
		export FILE_SIZE="${mb}M"
		expected_time_ms=$(( mb * 1000 / expected_time_divisor ))

		export MESSAGE="Writing $mb MB at $limit_mbps MB/s"
		echo "$MESSAGE"
		seconds=$("$time" -f %e file-generate "file_${mb}" 2>&1)
		actual_time_ms=$(bc <<< "scale=0; $seconds * 1000 / 1")
		assert_limited_io_time "${expected_time_ms}" "${actual_time_ms}"

		export MESSAGE="Reading $mb MB at $limit_mbps MB/s"
		echo "$MESSAGE"
		seconds=$("$time" -f %e file-validate "file_${mb}" 2>&1)
		actual_time_ms=$(bc <<< "scale=0; $seconds * 1000 / 1")
		assert_limited_io_time "${expected_time_ms}" "${actual_time_ms}"

		export MESSAGE="Reading + writing $mb MB at $limit_mbps MB/s"
		echo "$MESSAGE"
		seconds=$("$time" -f %e bash -c "file-validate file_${mb} & file-generate garbage & wait" 2>&1)
		actual_time_ms=$(bc <<< "scale=0; $seconds * 1000 / 1")
		assert_limited_io_time "$((2 * expected_time_ms))" "${actual_time_ms}"
	done
}

CHUNKSERVERS=3 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER|sfsiolimits=$iolimits" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

run_io_test 1 1

# check that iolimits could be changed at runtime using .saunafs_tweaks file
echo "IOLimitsFilePath=${iolimits2}" | sudo tee "${info[mount0]}/.saunafs_tweaks"
sleep 1 # wait a bit for the change to be reloaded on client

run_io_test 2 2

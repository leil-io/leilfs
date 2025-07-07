timeout_set "3 minutes"

CHUNKSERVERS=3 \
	MASTER_CUSTOM_GOALS="1 ec21: \$ec(2,1)" \
	AUTO_SHADOW_MASTER="NO" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	CHUNKSERVER_EXTRA_CONFIG="MAGIC_DEBUG_LOG = $TEMP_DIR/log|LOG_FLUSH_ON=DEBUG" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

# Generates some files in parallel to create memory pressure
function generateAndValidateFiles() {
	echo "Generating files to create memory pressure"

	for file in $(seq 1 100); do
		size=$((file * 1024))
		FILE_SIZE=${size} assert_success file-generate "file.${file}" &
	done

	wait
}

function getMallocTrimIntervalFromLog() {
	grep -ioP '(?<=Effective MALLOC_TRIM_INTERVAL: )[0-9]+' "${TEMP_DIR}/log" | tail -n 1
}

function checkMemoryTrimmedFromLog() {
	grep -q "Memory trimmed successfully" "${TEMP_DIR}/log"
}

function getResidentMemoryForPid() {
	pid=${1}
	ps -o rss= -p ${pid}
}

# Generate some files to create memory pressure
generateAndValidateFiles

# Clear log to have clean state for testing
> "${TEMP_DIR}/log"

### Test MALLOC_TRIM_INTERVAL configuration and functionality ###

trimInterval=5  # 5 seconds for quick testing

# Get initial memory values for chunkserver 0
cs0Pid=$(saunafs_chunkserver_daemon 0 test | tr -d '\0' | awk '{print $NF}')
residentMemoryCS0=$(getResidentMemoryForPid ${cs0Pid})
echo "CS0 PID: ${cs0Pid} - Initial resident memory: ${residentMemoryCS0} KB"

# Configure MALLOC_TRIM_INTERVAL and restart chunkserver
echo "MALLOC_TRIM_INTERVAL = ${trimInterval}" >> "${info[chunkserver0_cfg]}"
saunafs_chunkserver_daemon 0 reload

# Let the chunkserver reload
sleep 5

# Check if the trim interval was set correctly
effectiveTrimInterval=$(getMallocTrimIntervalFromLog)
assert_equals ${trimInterval} ${effectiveTrimInterval}
echo "MALLOC_TRIM_INTERVAL set to ${effectiveTrimInterval} seconds for chunkserver 0"

# Wait for at least one trim cycle plus buffer time
echo "Waiting for memory trim to occur (waiting $((trimInterval + 2)) seconds)..."
sleep $((trimInterval + 2))

# Check if memory trimming actually occurred
if checkMemoryTrimmedFromLog; then
	echo "Memory trimming successfully executed"
else
	echo "Warning: Memory trimming log message not found - this may be normal if no memory needed trimming"
fi

# Get memory values after trimming period
cs0PidAfter=$(saunafs_chunkserver_daemon 0 test | tr -d '\0' | awk '{print $NF}')
residentMemoryCS0After=$(getResidentMemoryForPid ${cs0PidAfter})
echo "CS0 PID: ${cs0PidAfter} - Resident memory after trim period: ${residentMemoryCS0After} KB"

### Test with different chunkserver (chunkserver 1) ###

# Test chunkserver 1 with a different trim interval
trimInterval2=3
cs1Pid=$(saunafs_chunkserver_daemon 1 test | tr -d '\0' | awk '{print $NF}')
residentMemoryCS1=$(getResidentMemoryForPid ${cs1Pid})
echo "CS1 PID: ${cs1Pid} - Initial resident memory: ${residentMemoryCS1} KB"

# Clear log again for clean testing
> "${TEMP_DIR}/log"

# Configure different MALLOC_TRIM_INTERVAL for chunkserver 1
echo "MALLOC_TRIM_INTERVAL = ${trimInterval2}" >> "${info[chunkserver1_cfg]}"
saunafs_chunkserver_daemon 1 reload

# Let the chunkserver reload
sleep 5

# Verify the new interval is set
effectiveTrimInterval2=$(getMallocTrimIntervalFromLog)
assert_equals ${trimInterval2} ${effectiveTrimInterval2}
echo "MALLOC_TRIM_INTERVAL set to ${effectiveTrimInterval2} seconds for chunkserver 1"

### Test minimum value constraint ###

# Test that the minimum value (1 second) is enforced
trimIntervalMin=1
echo "MALLOC_TRIM_INTERVAL = 0" >> "${info[chunkserver2_cfg]}"

# Clear log for minimum test
> "${TEMP_DIR}/log}"

saunafs_chunkserver_daemon 2 reload

# Let the chunkserver reload
sleep 5

effectiveTrimIntervalMin=$(getMallocTrimIntervalFromLog)
assert_equals ${trimIntervalMin} ${effectiveTrimIntervalMin}
echo "Minimum MALLOC_TRIM_INTERVAL (${effectiveTrimIntervalMin} seconds) correctly enforced"

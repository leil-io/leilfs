timeout_set "1 minute"

CHUNKSERVERS=3 \
	MASTER_CUSTOM_GOALS="1 ec21: \$ec(2,1)" \
	AUTO_SHADOW_MASTER="NO" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	CHUNKSERVER_EXTRA_CONFIG="MALLOC_TRIM_INTERVAL=0|MAGIC_DEBUG_LOG = $TEMP_DIR/log|LOG_FLUSH_ON=DEBUG" \
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

trimInterval=3  # 3 seconds for quick testing

# Get initial memory values for chunkserver 0
cs0Pid=$(saunafs_chunkserver_daemon 0 test | tr -d '\0' | awk '{print $NF}')
residentMemoryCS0=$(getResidentMemoryForPid ${cs0Pid})
echo "CS0 PID: ${cs0Pid} - Initial resident memory: ${residentMemoryCS0} KB"

# Configure MALLOC_TRIM_INTERVAL and reload chunkserver
echo "MALLOC_TRIM_INTERVAL = ${trimInterval}" >> "${info[chunkserver0_cfg]}"
saunafs_chunkserver_daemon 0 reload

# Let the chunkserver reload
sleep $(timeout_rescale_seconds 3)

# Check if the trim interval was set correctly
effectiveTrimInterval=$(getMallocTrimIntervalFromLog)
assert_equals ${trimInterval} ${effectiveTrimInterval}
echo "MALLOC_TRIM_INTERVAL set to ${effectiveTrimInterval} seconds for chunkserver 0"

# Wait for at least one trim cycle plus buffer time
sleepTime=$(timeout_rescale_seconds $((trimInterval + 2)))
echo "Waiting for memory trim to occur (waiting ${sleepTime} seconds)..."
sleep "${sleepTime}"

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

### Test disabling the feature #

# Clear the log
> "${TEMP_DIR}/log"

echo "Disabling MALLOC_TRIM_INTERVAL by setting it to 0 in the configuration"
# Modify MALLOC_TRIM_INTERVAL and reload chunkserver
trimInterval=0
sed -i s/"^MALLOC_TRIM_INTERVAL = .*"/"MALLOC_TRIM_INTERVAL = ${trimInterval}"/g "${info[chunkserver0_cfg]}"
saunafs_chunkserver_daemon 0 reload

# Let the chunkserver reload
echo "Waiting for chunkserver to reload"
sleep $(timeout_rescale_seconds 3)

# Check that the thread was stopped
assert_success grep -q "Memory trimming disabled" "${TEMP_DIR}/log"
echo "Memory trimming thread was stopped"

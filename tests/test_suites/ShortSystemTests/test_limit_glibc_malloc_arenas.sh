timeout_set "2 minutes"

CHUNKSERVERS=3 \
	MASTER_CUSTOM_GOALS="1 ec21: \$ec(2,1)" \
	AUTO_SHADOW_MASTER="NO" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	CHUNKSERVER_EXTRA_CONFIG="MAGIC_DEBUG_LOG = $TEMP_DIR/log|LOG_FLUSH_ON=DEBUG" \
	MASTER_EXTRA_CONFIG="MAGIC_DEBUG_LOG = $TEMP_DIR/log|LOG_FLUSH_ON=DEBUG" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

# Generates some files in parallel
function generateAndValidateFiles() {
	echo "Generating the files"

	for file in $(seq 1 100); do
		size=$((file * 1024))
		FILE_SIZE=${size} assert_success file-generate "file.${file}" &
	done

	wait
}

function getActualArenaLimitFromLog() {
	grep -ioP '(?<=Setting glibc malloc arena max to )[0-9]+' "${TEMP_DIR}/log" | tail -n 1
}

function getVirtualMemoryForPid() {
	pid=${1}
	ps -o vsz= -p ${pid}
}

# Generate some files to populate the filesystem
generateAndValidateFiles

# Master is consuming more memory after restart, so the comparison is not fair.
# This restart makes the later comparison in better conditions.
saunafs_master_daemon restart
saunafs_wait_for_all_ready_chunkservers

arenas=2

### Test the limit in the first chunkserver ###

# Get initial values
cs0Pid=$(saunafs_chunkserver_daemon 0 test | tr -d '\0' | awk '{print $NF}')
virtualMemoryCS0=$(getVirtualMemoryForPid ${cs0Pid})
echo "CS0 PID: ${cs0Pid} - Virtual memory: ${virtualMemoryCS0}"

# Edit the configuration to set the limit and restart
echo "LIMIT_GLIBC_MALLOC_ARENAS = ${arenas}" >> "${info[chunkserver0_cfg]}"
saunafs_chunkserver_daemon 0 restart
saunafs_wait_for_all_ready_chunkservers

# Check if the limit was set
effectiveArenaLimit=$(getActualArenaLimitFromLog)
assert_equals ${arenas} ${effectiveArenaLimit}
echo "Arena limit set to ${effectiveArenaLimit} for chunkserver 0"

# Check if the memory usage decreased
cs0PidAfter=$(saunafs_chunkserver_daemon 0 test | tr -d '\0' | awk '{print $NF}')
virtualMemoryCS0After=$(getVirtualMemoryForPid ${cs0PidAfter})
echo "CS0 PID: ${cs0PidAfter} - Virtual memory: ${virtualMemoryCS0After}"
assert_less_or_equal ${virtualMemoryCS0After} ${virtualMemoryCS0}

### Test the limit in the master ###

# Get initial values
master0Pid=$(saunafs_master_n 0 test | tr -d '\0' | awk '{print $NF}')
virtualMemoryMaster0=$(getVirtualMemoryForPid ${master0Pid})
echo "Master0 PID: ${master0Pid} - Virtual memory: ${virtualMemoryMaster0}"

# Edit the configuration to set the limit and restart
echo "LIMIT_GLIBC_MALLOC_ARENAS = ${arenas}" >> "${info[master0_cfg]}"
saunafs_master_daemon restart
saunafs_wait_for_all_ready_chunkservers

# Check if the limit was set
effectiveArenaLimit=$(getActualArenaLimitFromLog)
assert_equals ${arenas} ${effectiveArenaLimit}
echo "Arena limit set to ${effectiveArenaLimit} for master 0"

# Check if the memory usage decreased
master0PidAfter=$(saunafs_master_n 0 test | tr -d '\0' | awk '{print $NF}')
virtualMemoryMaster0After=$(getVirtualMemoryForPid ${master0PidAfter})
echo "Master0 PID: ${master0PidAfter} - Virtual memory: ${virtualMemoryMaster0After}"
assert_less_or_equal ${virtualMemoryMaster0After} ${virtualMemoryMaster0}

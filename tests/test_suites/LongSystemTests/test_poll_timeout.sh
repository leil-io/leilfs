timeout_set "3 minutes"

CHUNKSERVERS=3 \
	MASTER_CUSTOM_GOALS="1 ec21: \$ec(2,1)" \
	AUTO_SHADOW_MASTER="NO" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	CHUNKSERVER_EXTRA_CONFIG="MAGIC_DEBUG_LOG = $TEMP_DIR/log|LOG_FLUSH_ON=DEBUG" \
	MASTER_EXTRA_CONFIG="MAGIC_DEBUG_LOG = $TEMP_DIR/log|LOG_FLUSH_ON=DEBUG" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

# Generate and validate some files in parallel
function generateAndValidateFiles() {
	echo "Generating and validating the files"

	for file in $(seq 1 20); do
		size=$((file * 1024 * 1024))
		FILE_SIZE=${size} assert_success file-generate "file.${file}" &
	done

	wait

	for file in $(seq 1 20); do
		assert_success file-validate "file.${file}" &
	done

	wait
}

function getActualPollTimeoutFromLog() {
	grep -ioP '(?<=poll timeout set to )[0-9]+' "${TEMP_DIR}/log" | tail -n 1
}

lastChunkserver=2

# Add default timeout to master and chunkservers cfgs
defaultTimeout=50

for cs in $(seq 0 ${lastChunkserver}); do
	echo "POLL_TIMEOUT_MS = ${defaultTimeout}" >> "${info[chunkserver${cs}_cfg]}"
	saunafs_chunkserver_daemon ${cs} restart
done

echo "POLL_TIMEOUT_MS = ${defaultTimeout}" >> "${info[master0_cfg]}"

previousMasterTimeout=${defaultTimeout}
previousChunkserverTimeout=${defaultTimeout}

function applyTimeouts() {
	local masterTimeout=$1
	local chunkserverTimeout=$2

	echo "Timeouts: Master=${masterTimeout}, Chunkservers=${chunkserverTimeout}"

	for cs in $(seq 0 ${lastChunkserver}); do
		sed -i "s/POLL_TIMEOUT_MS = ${previousChunkserverTimeout}/POLL_TIMEOUT_MS = ${chunkserverTimeout}/" \
		    "${info[chunkserver${cs}_cfg]}"
		saunafs_chunkserver_daemon ${cs} restart
	done

	saunafs_wait_for_all_ready_chunkservers

	lastTimeout=$(getActualPollTimeoutFromLog)
	assert_equals "${chunkserverTimeout}" "${lastTimeout}"

	sed -i "s/POLL_TIMEOUT_MS = ${previousMasterTimeout}/POLL_TIMEOUT_MS = ${masterTimeout}/" \
	    "${info[master0_cfg]}"
	saunafs_master_daemon restart

	saunafs_wait_for_all_ready_chunkservers

	lastTimeout=$(getActualPollTimeoutFromLog)
	assert_equals "${masterTimeout}" "${lastTimeout}"

	previousChunkserverTimeout=${chunkserverTimeout}
	previousMasterTimeout=${masterTimeout}
}

# Check different combinations of timeouts (from almost non-blocking to big wait)
timeouts=(10 25 75 100)

for masterTimeout in "${timeouts[@]}"; do
	for chunkserverTimeout in "${timeouts[@]}"; do
		applyTimeouts "${masterTimeout}" "${chunkserverTimeout}"
		generateAndValidateFiles
	done
done

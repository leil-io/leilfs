CHUNKSERVERS=3 \
	USE_RAMDISK=YES \
	MASTER_CUSTOM_GOALS="1 ec21: \$ec(2,1)" \
	AUTO_SHADOW_MASTER="NO" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	CHUNKSERVER_EXTRA_CONFIG="MAGIC_DEBUG_LOG = $TEMP_DIR/log|LOG_FLUSH_ON=INFO" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

# Generate and validate some files in parallel
function generateAndValidateFiles() {
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

function getActualWorkers() {
	grep -oP '(?<=master connection: )[0-9]+(?= background workers created)' "${TEMP_DIR}/log" | \
	tail -n 1
}

function getActualReplicationWorkers() {
	grep -oP '(?<=master connection: )[0-9]+' "${TEMP_DIR}/log" | \
	tail -n 1
}

lastChunkserver=2

# Minimum workers
minimumWorkers=2

# Default replication workers
defaultReplicationWorkers=5

## Set number of workers to 0 by configuration in all chunkservers and restart
for cs in $(seq 0 ${lastChunkserver}); do
	echo "MASTER_NR_OF_WORKERS = 0" >> "${info[chunkserver${cs}_cfg]}"
	saunafs_chunkserver_daemon ${cs} restart
done

saunafs_wait_for_all_ready_chunkservers

## Check that the system is still able to write and read files
generateAndValidateFiles

## Even if set to 0, the minimum number of workers is 2 in the code
actualWorkers=$(getActualWorkers)
assert_equals "${minimumWorkers}" "${actualWorkers}"
assert_equals "${defaultReplicationWorkers}" "$(getActualReplicationWorkers)"

# High number of workers
highWorkers=50

## Substitute the number of workers to be 50 in all chunkservers and restart
for cs in $(seq 0 ${lastChunkserver}); do
	sed -i 's/MASTER_NR_OF_WORKERS = 0/MASTER_NR_OF_WORKERS = 50/' \
		"${info[chunkserver${cs}_cfg]}"
	saunafs_chunkserver_daemon ${cs} restart
done

saunafs_wait_for_all_ready_chunkservers

## Check that the system is still able to write and read files
generateAndValidateFiles

actualWorkers=$(getActualWorkers)
assert_equals "${highWorkers}" "${actualWorkers}"

timeout_set 90 seconds

# Use goal 2 to increase the chance of overwhelming the chunkserver read queues
# Lots of readahead requests should be sent to chunkservers with very low timeout, and we want
# to make sure that the chunkserver read scheme can handle this without serious issues.

CHUNKSERVERS=2 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER|maxreadaheadrequests=10|readaheadmaxwindowsize=65536|` \
		`sfschunkserverwavereadto=30|sfschunkserverconnectreadto=30|sfschunkservertotalreadto=30" \
	CHUNKSERVER_EXTRA_CONFIG="MAX_PARALLEL_HDD_READ_JOBS_PER_CS_ENTRY=8|`
		`MAGIC_DEBUG_LOG=${TEMP_DIR}/log" \
	setup_local_empty_saunafs info

cd ${info[mount0]}

mkdir dir_rep2
saunafs setgoal -r 2 dir_rep2

# Create a big file that will be read in many chunks
FILE_SIZE=$((900 * 1024 * 1024)) BLOCK_SIZE=12345 file-generate dir_rep2/file

for i in $(seq 1 20); do
	if ! file-validate dir_rep2/file; then
		test_add_failure "File validation failed, retrying (iter: ${i})"
	fi
	echo "File validation succeeded (iter: ${i})"
done

if grep -q "refcount = 0 - This should never happen!" "${TEMP_DIR}/log"; then
	test_add_failure "Fatal: refcount = 0 - This should never happen! found in chunkserver logs"
fi

# Search for "[error]" in the chunkserver logs
if grep -F '[error]' "${TEMP_DIR}/log"; then
	test_add_failure "Fatal: Found errors in the chunkserver logs!"
fi

sfschunkserver_check_no_buffer_in_use

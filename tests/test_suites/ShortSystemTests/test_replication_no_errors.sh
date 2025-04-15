timeout_set 10 minutes

CHUNKSERVERS=5 \
	DISK_PER_CHUNKSERVER=1 \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	USE_RAMDISK=YES \
	MASTER_EXTRA_CONFIG="CHUNKS_LOOP_MIN_TIME = 10`
		`|CHUNKS_LOOP_MAX_CPU = 90`
		`|CHUNKS_WRITE_REP_LIMIT = 20`
		`|CHUNKS_READ_REP_LIMIT = 20`
		`|CHUNKS_SOFT_DEL_LIMIT = 5`
		`|CHUNKS_HARD_DEL_LIMIT = 5`
		`|OPERATIONS_DELAY_INIT = 5`
		`|OPERATIONS_DELAY_DISCONNECT = 5`
		`|ACCEPTABLE_DIFFERENCE = 0.01" \
	MASTER_CUSTOM_GOALS="10 ec31: \$ec(3,1)" \
	CHUNKSERVER_EXTRA_CONFIG="MAGIC_DEBUG_LOG = $TEMP_DIR/log|MASTER_NR_OF_WORKERS = 2" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"
mkdir dir
saunafs setgoal ec21 dir
cd dir

for i in {1..1000}; do
    # create a file of 1MB with zeroes
    dd if=/dev/zero of="file.$i" bs=1MiB count=1 status=none
done

# Change the goal of the directory to ec31
saunafs setgoal ec31 -r .

sleep 120

# Check the chunkserver logs for errors
# On unsuccessful runs the errors should be created when replicating parts from one goal
# to the other
cat "$TEMP_DIR/log"
assert_failure grep -q "hddChunkGetNumberOfBlocks: Couldn't find chunkID" "$TEMP_DIR/log"
assert_failure grep -q "hddOpen: could not find chunkid" "$TEMP_DIR/log"

list_cs_info=$(saunafs-admin list-chunkservers localhost "${info[matocl]}")
# Check that all chunkservers didn't have errors
assert_equals "$(echo "${list_cs_info}" | grep -c 'errors: 0')" "${info[chunkserver_count]}"

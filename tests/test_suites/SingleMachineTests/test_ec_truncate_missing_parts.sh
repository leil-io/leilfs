timeout_set '3 minutes'

CHUNKSERVERS=10 \
	DISK_PER_CHUNKSERVER=1 \
	MASTER_CUSTOM_GOALS="10 ec21: \$ec(2,1)`
	`|11 ec31: \$ec(3,1)`
	`|12 ec41: \$ec(4,1)`
	`|13 ec71: \$ec(7,1)`
	`|14 ec91: \$ec(9,1)" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

# List of file sizes which will be tested for each xor level
sizes=(100 200 300 1000 2000 3000 10000 20000 30000 100000 200000 300000 \
	$((SAUNAFS_BLOCK_SIZE - 1)) $((8 * SAUNAFS_BLOCK_SIZE)) $((50 * SAUNAFS_BLOCK_SIZE + 7)) \
	$((SAUNAFS_CHUNK_SIZE - 500)) $((SAUNAFS_CHUNK_SIZE + 500)) \
)

# List of xor levels which will be tested
levels=(2 3 4 7 9)

# For each ec level and each file size generate file of this size (using file-generate) and
# append some random amount of random bytes to it. Then make snapshot of such a file.
pseudorandom_init
cd "${info[mount0]}"
for i in "${levels[@]}"; do
	mkdir "ec${i}1"
	saunafs setgoal "ec${i}1" "ec${i}1"
	for size in "${sizes[@]}"; do
		FILE_SIZE=$size file-generate "ec${i}1/file_${size}"
		assert_success file-validate "ec${i}1/file_${size}"
		head -c $(pseudorandom 1 $((i * 100000))) /dev/urandom >> "ec${i}1/file_${size}"
		saunafs makesnapshot "ec${i}1/file_${size}" "ec${i}1/snapshot_${size}"
	done
done

# Now remove one of chunkservers
saunafs_chunkserver_daemon 0 stop

# For each created file restore its original size using truncate (ie. chop off the random bytes
# appended after generating the file) and verify if the data is OK.
for i in "${levels[@]}"; do
	for size in "${sizes[@]}"; do
		MESSAGE="Truncating ec${i}1 from $(stat -c %s ec${i}1/file_${size}) bytes to ${size} bytes"
		assert_success truncate -s ${size} "ec${i}1/snapshot_${size}"
		assert_success truncate -s ${size} "ec${i}1/file_${size}"
		assert_success file-validate "ec${i}1/file_${size}" "ec${i}1/snapshot_${size}"
	done
done

# Verify again after starting the chunkserver
saunafs_chunkserver_daemon 0 start
saunafs_wait_for_ready_chunkservers 10
for i in "${levels[@]}"; do
	for size in "${sizes[@]}"; do
		MESSAGE="Verification after starting the chunkserver" \
		assert_success file-validate "ec${i}1/file_${size}" "ec${i}1/snapshot_${size}"
	done
done

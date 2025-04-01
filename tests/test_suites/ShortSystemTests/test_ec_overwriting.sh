timeout_set 1 minute
CHUNKSERVERS=4 \
	DISK_PER_CHUNKSERVER=1 \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	MASTER_CUSTOM_GOALS="10 ec_3_1: \$ec(3,1)" \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

dir="${info[mount0]}/dir"
mkdir "$dir"
saunafs setgoal ec_3_1 "$dir"

# Create a temporary file with some data
file_size_mb=5
tmpf=$RAMDISK_DIR/tmpf
FILE_SIZE=${file_size_mb}M file-generate "$tmpf"

# Create a file on SaunaFS filesystem with random data
dd if=/dev/urandom of="$dir/file" bs=1MiB count=$file_size_mb

# Overwrite the file using data from the temporary file
# Use 20 parallel threads, each of them overwrites a random 1 KB block
seq 0 $((file_size_mb*1024-1)) | shuf | xargs -P20 -IXX \
		dd if="$tmpf" of="$dir/file" bs=1K count=1 seek=XX skip=XX conv=notrunc 2>/dev/null

# Validate in the usual way
if ! file-validate "$dir/file"; then
	test_add_failure "Data read from file is different than written"
fi

# Find the chunkserver serving part 1 of 3 and stop it
csid=$(find_first_chunkserver_with_chunks_matching 'chunk_ec2_1_of_3*')
saunafs_chunkserver_daemon $csid stop

# Validate the parity part
if ! file-validate "$dir/file"; then
	test_add_failure "Data read from file (with broken chunkserver) is different than written"
fi

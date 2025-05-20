# Checks that the data written to standard replication goals greater than 1 is properly forwarded to
# the next chunkserver in the chain.

timeout_set 1 minute

CHUNKSERVERS=2 \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

mkdir goal2
saunafs setgoal 2 goal2

file=random_write.file
size="80"

if [[ ${USE_VALGRIND} ]]; then
	size="32"
fi

echo "Creating a file with size ${size}M in tmp"
dd if=/dev/random of=${TEMP_DIR}/${file} bs=1M count=${size} oflag=direct status=none

original_md5="$(md5sum "${TEMP_DIR}/${file}" | awk '{print $1}')"
echo "md5_original: ${original_md5}"

# Copy de file to the mount point with goal 2
cp "${TEMP_DIR}/${file}" "./goal2/${file}"

# Stop chunkserver 1
saunafs_chunkserver_daemon 1 stop
saunafs_wait_for_ready_chunkservers 1

drop_caches

# Check if the file is matches the original md5, reading only from chunkserver 0
md5_sum="$(md5sum "./goal2/${file}" | awk '{print $1}')"
echo "md5_sum: ${md5_sum} - From chunkserver 0"
assert_equals "${original_md5}" "${md5_sum}"

drop_caches

# Stop chunkserver 0 and start chunkserver 1
saunafs_chunkserver_daemon 0 stop
saunafs_chunkserver_daemon 1 start
saunafs_wait_for_ready_chunkservers 1

# Check if the file is matches the original md5, reading only from chunkserver 1
md5_sum="$(md5sum "./goal2/${file}" | awk '{print $1}')"
echo "md5_sum: ${md5_sum} - From chunkserver 1"
assert_equals "${original_md5}" "${md5_sum}"

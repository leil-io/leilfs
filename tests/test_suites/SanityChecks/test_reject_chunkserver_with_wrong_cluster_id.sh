CHUNKSERVERS=2 \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

echo "Creating files on the mount point"

for fileNum in $(seq 1 5); do
	dd if=/dev/zero of=file${fileNum} bs=1M count=1 oflag=direct &> /dev/null
done

# Both chunkservers should be ready
assert_equals 2 $(saunafs_ready_chunkservers_count)

echo "Stopping chunkserver 1"
saunafs_chunkserver_daemon 1 stop

echo "Changing CLUSTER_ID to wrong one"
echo "CLUSTER_ID = cluster_02" >> "${info[chunkserver1_cfg]}"

echo "Starting chunkserver 1 with wrong CLUSTER_ID"
saunafs_chunkserver_daemon 1 start

for i in $(seq 1 5); do
	sleep 2
	echo "Number of chunkservers: $(saunafs_ready_chunkservers_count)"
	assert_equals 1 $(saunafs_ready_chunkservers_count)
done

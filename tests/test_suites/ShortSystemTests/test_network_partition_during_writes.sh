# Test client behavior during network partition
timeout_set 2 minute

CHUNKSERVERS=4 \
	MASTER_CUSTOM_GOALS="10 ec_3_1: \$ec(3,1)" \
	setup_local_empty_saunafs info

mkdir "${info[mount0]}/dir"
saunafs settrashtime 0 "${info[mount0]}/dir"

FILE_SIZE_MB=1024
FILE_NAME="${info[mount0]}/dir/network_test_file"
FILE_NAME_CPY="${FILE_NAME}_cpy"
touch ${FILE_NAME} ${FILE_NAME_CPY}
saunafs setgoal ec_3_1 ${FILE_NAME} ${FILE_NAME_CPY}

# Create a file with FILE_SIZE_MB MB of generated data
FILE_SIZE=${FILE_SIZE_MB}M file-generate ${FILE_NAME}

# Let's try only stopping the master
# Start cpy operation
( dd if=${FILE_NAME} of=${FILE_NAME_CPY} bs=1M count=${FILE_SIZE_MB} status=none ) &

# Allow write to start
sleep $(timeout_rescale_seconds 1)

# Simulate network partition by stopping master
echo "Stopping master to simulate network partition."
saunafs_master_daemon stop

# Sleep for a while to ensure the write tries to complete
# After a few second wake up the master
sleep $(timeout_rescale_seconds 10)

saunafs_master_daemon restart
echo "Done simulating network partition."

# Wait for write to complete
wait

if [ $? -eq 0 ]; then
    echo "Write succeeded during network partition."
else
    test_add_failure "Write failed unexpectedly during network partition."
fi

assert_success file-validate ${FILE_NAME_CPY}
echo "Write continues after master crashed and restarted: OK"

# Cleanup
rm ${FILE_NAME_CPY}

# Let's try only stopping the chunkservers
# Start cpy operation
( dd if=${FILE_NAME} of=${FILE_NAME_CPY} bs=1M count=${FILE_SIZE_MB} status=none ) &

# Allow write to start
sleep $(timeout_rescale_seconds 1)

# Simulate chunkserver failures
for i in {0..3}; do
	echo "Chunkserver ${i} stopped."
	saunafs_chunkserver_daemon ${i} stop
done

# Sleep for a while to ensure the write tries to complete
# After a few second wake up the CSs
sleep $(timeout_rescale_seconds 10)

# Restart chunkservers
for i in {0..3}; do
	echo "Chunkserver ${i} started."
	saunafs_chunkserver_daemon ${i} start
done
echo "Done simulating network partition."

# Wait for write to complete
wait

if [ $? -eq 0 ]; then
    echo "Write succeeded during network partition."
else
    test_add_failure "Write failed unexpectedly during network partition."
fi

assert_success file-validate ${FILE_NAME_CPY}
echo "Write continues after chunkservers crashed and restarted: OK"

# Cleanup
rm ${FILE_NAME_CPY}

# Let's try stopping the chunkservers and master
# Start cpy operation
( dd if=${FILE_NAME} of=${FILE_NAME_CPY} bs=1M count=${FILE_SIZE_MB} status=none ) &

# Allow write to start
sleep $(timeout_rescale_seconds 1)

# Simulate master and chunkserver failures
echo "Stopping master to simulate network partition."
saunafs_master_daemon stop
for i in {0..3}; do
	echo "Chunkserver ${i} stopped."
	saunafs_chunkserver_daemon ${i} stop
done

# Sleep for a while to ensure the write tries to complete
# After a few second wake up the CSs
sleep $(timeout_rescale_seconds 10)

# Restart chunkservers and master
for i in {0..3}; do
	echo "Chunkserver ${i} started."
	saunafs_chunkserver_daemon ${i} start
done
saunafs_master_daemon restart
echo "Done simulating network partition."

# Wait for write to complete
wait

if [ $? -eq 0 ]; then
    echo "Write succeeded during network partition."
else
    test_add_failure "Write failed unexpectedly during network partition."
fi

assert_success file-validate ${FILE_NAME_CPY}
echo "Write continues after chunkservers and master crashed and restarted: OK"

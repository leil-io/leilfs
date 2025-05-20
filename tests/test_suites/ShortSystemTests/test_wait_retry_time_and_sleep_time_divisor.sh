timeout_set "5 minutes"

run_ls_background() {
    local suffix="$1"

    # start the ls command in the background and redirect
    # its output to a file named ls_output_<suffix>.txt
    (
      ls -l "${info[mount0]}" > "/tmp/ls_output_${suffix}.txt" 2>&1
    ) &
}

USE_RAMDISK=YES \
	CHUNKSERVERS=1 \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER,sfsioretries=10,maxwaitretrytime=3,mastercommsleeptimedivisor=2" \
	setup_local_empty_saunafs info

# stop master server daemon
saunafs_master_daemon stop

# start the ls command in the background with suffix "1"
run_ls_background "1"

# wait for a duration longer than the maximum time the 
# client will wait for the master server to return.
# The calculation is as follows:
# The client performs up to 10 retries, waiting a period
# that increases by 1 second every "mastercommsleeptimedivisor"
# iterations (in this case, every 2 iterations), up to a 
# maximum wait time of 3 seconds per retry. 
# This results in a total wait time of 1 + 1 + 2 + 2 + 3 + 3 + 3*4 = 24 seconds
# per operation. Now, given that the client will perform a
# SAU_MATOCL_UPDATE_CREDENTIALS and a MATOCL_FUSE_GETATTR operation for completing
# the ls command, we need to wait for more than 48 seconds
# Then we will wait for 50 seconds to reconnect the master server
# and check that client could not successfully finish the ls command
# before the master server is back online.
sleep 50
saunafs_master_daemon start
saunafs_wait_for_ready_chunkservers 1

# at this time, the ls command should have failed
assert_success grep -q "Input/output error" "/tmp/ls_output_1.txt"

# now check the case when the master server is back online
# on a time less than the maximum time the client will wait
# for the master server to return

# stop master server daemon
saunafs_master_daemon stop

# start the ls command in the background with suffix "2"
run_ls_background "2"

# wait for a duration shorter than the maximum time the
# client will wait for the master server to return a response
# for the first operation during the ls command, which is
# SAU_MATOCL_UPDATE_CREDENTIALS. Then if we reconnect the
# master server before first 24 seconds, the ls command should
# be able to finish successfully.
sleep 12
saunafs_master_daemon start
saunafs_wait_for_ready_chunkservers 1

# at this time, the ls command should have succeeded
# and created the file ls_output_2.txt as expected
assert_failure grep -q "Input/output error" "/tmp/ls_output_2.txt"

# Add a delay of 5 seconds to ensure that cluster is stable
sleep 5

# now check changing the values of sfsioretries, maxwaitretrytime
# and mastercommsleeptimedivisor using the tweaks file
echo "MaxRetriesMasterComm=5" | sudo tee "${info[mount0]}/.saunafs_tweaks"
echo "MaxWaitRetryTimeMasterComm=5" | sudo tee "${info[mount0]}/.saunafs_tweaks"
echo "MasterCommSleepTimeDivisor=1" | sudo tee "${info[mount0]}/.saunafs_tweaks"

# Check if tweaks are correctly updated
assert_equals "5" $(cat "${info[mount0]}/.saunafs_tweaks" | egrep MaxRetriesMasterComm | awk '{print $2}')
assert_equals "5" $(cat "${info[mount0]}/.saunafs_tweaks" | egrep MaxWaitRetryTimeMasterComm | awk '{print $2}')
assert_equals "1" $(cat "${info[mount0]}/.saunafs_tweaks" | egrep MasterCommSleepTimeDivisor | awk '{print $2}')

# stop master server daemon
saunafs_master_daemon stop

# start the ls command in the background with suffix "3"
run_ls_background "3"

# wait for a duration longer than the maximum time the
# client will wait for the master server to return.
# The calculation is as follows:
# The client performs up to 1 + 2 + 3 + 4 + 5 = 15 seconds for
# each failing operation. Then, for this case, we need to wait
# for more than 30 seconds to check that the ls command
# could not finish before the master server is back online.
sleep 32
saunafs_master_daemon start
saunafs_wait_for_ready_chunkservers 1

# at this time, the ls command should have failed
assert_success grep -q "Input/output error" "/tmp/ls_output_3.txt"

# now check the case when the master server is back online
# on a time less than the maximum time the client will wait
# for the master server to return
saunafs_master_daemon stop

# start the ls command in the background with suffix "4"
run_ls_background "4"

# wait for a duration shorter than the maximum time the
# client will wait for the master server to return a response
# for the first operation during the ls command, which is
# SAU_MATOCL_UPDATE_CREDENTIALS.
sleep 15
saunafs_master_daemon start
saunafs_wait_for_ready_chunkservers 1

# if options maxwaitretrytime and mastercommsleeptimedivisor would
# have default values (10 and 3 respectively), the ls command would have
# failed.
# but since we set them to 5 and 1 respectively, the ls command
# should have succeeded as expected.
assert_failure grep -q "Input/output error" "/tmp/ls_output_4.txt"

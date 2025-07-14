timeout_set 45 seconds

# Tests if chunkservers print correct when trying to connect to old masters (v4.*)

export SAFS_MOUNT_COMMAND="sfsmount"

CHUNKSERVERS=1 \
	MOUNTS=1 \
	START_WITH_LEGACY_SAUNAFS=YES \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	CHUNKSERVER_EXTRA_CONFIG="MASTER_RECONNECTION_DELAY=2|MAGIC_DEBUG_LOG=$TEMP_DIR/log|LOG_FLUSH_ON=DEBUG" \
	setup_local_empty_saunafs info

# Start test with master, 1 chunkserver and 1 mount running legacy SaunaFS code
# Ensure that we work on legacy version
assert_equals 1 $(saunafs_old_admin_master info | grep $SAUNAFSXX_TAG | wc -l)
assert_equals 1 $(saunafs_old_admin_master list-chunkservers | grep $SAUNAFSXX_TAG | wc -l)
assert_equals 1 $(saunafs_old_admin_master list-mounts | grep $SAUNAFSXX_TAG | wc -l)

cd "${info[mount0]}"

# Stop old chunkserver and start the new one
saunafsXX_chunkserver_daemon 0 stop
saunafs_chunkserver_daemon 0 start

saunafs_wait_for_all_ready_chunkservers

# Check if chunkserver log contains the expected messages

logsNum=$(grep -c "Master server did not answer the registration request" "${TEMP_DIR}/log")
assert_not_equal 0 ${logsNum}

oldRegistrationAttempts=$(grep -c "using old master registration" "${TEMP_DIR}/log")
assert_equals 1 ${oldRegistrationAttempts}

# Restart the master to trigger a chunkserver reconnection
saunafsXX_master_daemon stop
saunafsXX_master_daemon start

saunafs_wait_for_all_ready_chunkservers

echo "Chunkserver reconnected successfully after Master restart"

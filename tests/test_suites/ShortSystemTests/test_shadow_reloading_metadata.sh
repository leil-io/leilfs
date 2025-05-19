timeout_set '1 minute'

CHUNKSERVERS=1 \
	MASTERSERVERS=2 \
	USE_RAMDISK="YES" \
	SFSEXPORTS_EXTRA_OPTIONS="allcanchangequota" \
	MASTER_EXTRA_CONFIG="MASTER_TIMEOUT = 10|METADATA_DUMP_PERIOD_SECONDS = 0" \
	setup_local_empty_saunafs info

# Generate any changes, connect shadow master and wait until it's fully synchronized
# Master should have metadata file with version 1, but after synchronization shadow will dump newer
touch "${info[mount0]}"/file
saunafs_master_n 1 start
assert_eventually "saunafs_shadow_synchronized 1"

# Make more changes, shadow should apply them
touch "${info[mount0]}"/file{1..10}
assert_eventually "saunafs_shadow_synchronized 1"

# Make shadow master lose connection with the master by making it sleep and restarting master server
shadow_pid=$(saunafs_master_n 1 test | sed 's/.*: //')
assert_matches "^[0-9]+$" "$shadow_pid"
kill -STOP "$shadow_pid"
saunafs_master_daemon restart

# Generate a lot of new changes and remove changelogs from shadow's version to master's version
touch "${info[mount0]}"/file{1..20}
saunafs_master_daemon stop
rm "${info[master0_data_path]}"/changelog*
saunafs_master_daemon start
touch "${info[mount0]}"/file{20..30}

# Make shadow master recover it's connection and expect shadow master to synchronize correctly
kill -CONT "$shadow_pid"
assert_eventually "saunafs_shadow_synchronized 1"

# Verify shadow has proper metadata
metadata=$(metadata_print "${info[mount0]}")
saunafs_master_daemon kill
saunafs_make_conf_for_master 1
saunafs_master_daemon reload
saunafs_wait_for_all_ready_chunkservers
assert_no_diff "$metadata" "$(metadata_print "${info[mount0]}")"

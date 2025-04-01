timeout_set 120 minutes

# We create file in some custom goal, e.g. '2', or ec21, or ec32.
# We close $CHUNKSERVERS_DOWN_N chunkservers, so that this file is in undergoal,
# and only minimum number of chunkservers for files to be readable remains on.
# (e.g. we close 2 chunkservers when goal=ec32, or 1 when goal=ec21/2).
# We overwrite the file, then restart and upgrade all SaunaFS services.
# New version should then remain, and be replicated to previously closed chunkservers.

# Variables passed from place where this templated is invoked:
# - GOAL (e.g. 2, ec21, ec32)
# - SAFS_MOUNT_COMMAND_AFTER_UPGRADE (mount command to use after
#                                     upgrade to current SaunaFS version was done)

source test_utils/upgrade.sh

CHUNKSERVERS=3
START_WITH_LEGACY_SAUNAFS=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	MASTER_EXTRA_CONFIG="CHUNKS_LOOP_MIN_TIME = 1|OPERATIONS_DELAY_INIT = 0" \
	setup_local_empty_saunafs info

REPLICATION_SPEED="5000000" # Bytes per second

# Start the test with master, two chunkservers and mount running old SaunaFS code
# Ensure that we work on legacy version
assert_saunafsXX_services_count_equals 1 ${CHUNKSERVERS} 1

cd "${info[mount0]}"
mkdir dir
assert_success saunafsXX saunafs setgoal $GOAL dir
cd dir

echo "Starting test"

# map filesize V-> cnt, e.g. 20M -> 5 (5 files of filesize 20M)
declare -A file_count
fill_files_info_maps_with_default_values file_count # always that, or/and add some custom values to arrays here

assert_success generate_files_various_filesizes file_count
echo "All files generated"

# Wait until chunk has been replicated
wait_for_files_replication file_count "${REPLICATION_SPEED}"
echo "All files replicated (3 parts of each chunk)"

# We stop 1 chunkserver, and overwrite files
saunafsXX_chunkserver_daemon 0 stop
echo "Chunkserver 0 stopped."

echo "Overwriting half files."
assert_success overwrite_half_files file_count

echo "Validating all files."
assert_success validate_all_files file_count
echo "All files validated."

echo "Listing mounts."
saunafs_admin_master list-mounts

# Restart all services, start in new version
cd "$TEMP_DIR"
saunafsXX_chunkserver_daemon 1 stop
saunafsXX_chunkserver_daemon 2 stop
assert_success saunafs_mount_unmount 0
saunafs_admin_master list-mounts
saunafs_master_daemon restart
saunafs_mount_start 0
saunafs_chunkserver_daemon 0 start
saunafs_chunkserver_daemon 1 start
saunafs_chunkserver_daemon 2 start

saunafs_wait_for_ready_chunkservers 3

# Ensure that versions are switched
assert_no_saunafsXX_services_active

echo "3.13 is On"
# Check if new content was preserved, not old one. On both chunkservers.
cd "${info[mount0]}/dir"
wait_for_files_replication file_count "${REPLICATION_SPEED}"

echo "ALL REPLICATED"

echo "Validating files with 3/3 CS on"
validate_all_files file_count

echo "Validating files with CS 1 2 on"
saunafs_chunkserver_daemon 0 stop
saunafs_wait_for_ready_chunkservers 2
validate_all_files file_count

echo "Validating files with CS 0 2 on"
saunafs_chunkserver_daemon 1 stop
saunafs_wait_for_ready_chunkservers 1
saunafs_chunkserver_daemon 0 start
saunafs_wait_for_ready_chunkservers 2
validate_all_files file_count

echo "Validating files with CS 0 1 on"
saunafs_chunkserver_daemon 2 stop
saunafs_wait_for_ready_chunkservers 1
saunafs_chunkserver_daemon 1 start
saunafs_wait_for_ready_chunkservers 2
validate_all_files file_count

saunafs_chunkserver_daemon 2 start
saunafs_wait_for_ready_chunkservers 3
sleep 25m
validate_all_files file_count


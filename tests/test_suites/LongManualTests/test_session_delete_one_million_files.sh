timeout_set 30 minutes

# Manual responsiveness check: delete a session with many files deleted while open
# (one million by default). A connected chunkserver must stay registered, and admin
# requests must complete during cleanup. Each release purges one reserved file,
# so the reserved-file count measures progress.
files=${SESSION_RELEASE_FILES:-1000000}
# Files one process holds open, below the open-file limit of the test processes.
files_per_holder=9000

# Suppress periodic open-inode reports, which can exceed the master's packet-size limit at
# this scale, so this test measures session cleanup.
CHUNKSERVERS=1 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfsreportreservedperiod=1000000" \
	CHUNKSERVER_EXTRA_CONFIG="MASTER_TIMEOUT = 2|MASTER_RECONNECTION_DELAY = 1|`
		`MAGIC_DEBUG_LOG=${TEMP_DIR}/chunkserver.log|LOG_FLUSH_ON=DEBUG" \
	setup_local_empty_saunafs info

source "${SOURCE_DIR}/tests/tools/session_release_helpers.sh"

session_release_hold_deleted_files "${info[mount0]}/held" "${files}" \
	"${files_per_holder}" subdirectories
setup_start=$(date +%s)
session_release_wait_for_holders '20 minutes'
echo "Session deletion: ${files} files open and deleted in $(($(date +%s) - setup_start)) s"
assert_equals "${files}" "$(session_release_reserved_files)"

session=$(session_release_session_id 0)
assert_matches '^[0-9]+$' "${session}"
samples="${TEMP_DIR}/samples"
session_release_start_timed_sampling "${samples}"

# A stopped mount cannot reconnect, so only the release frees the session's files.
mount0=$(session_release_mount_process 0)
assert_matches '^[0-9]+$' "${mount0}"
assert_success kill -STOP "${mount0}"
session_release_timed_delete "${session}"
session_release_wait_for_sampling
assert_success kill -KILL "${mount0}"
assert_success saunafs_fusermount -u -z "${info[mount0]}"

session_release_report_timed_samples 'Session deletion' "${files}" "${samples}"
expect_equals 0 "${session_release_delete_status}"
expect_awk_finds '$0 == "Session deleted successfully"' "${session_release_delete_reply}"
session_release_expect_timed_responsiveness
session_release_cleanup

timeout_set 30 minutes

# Manual responsiveness check: expire a session with many files deleted while open
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
	MASTER_EXTRA_CONFIG="SESSION_SUSTAIN_TIME=60" \
	MOUNT_EXTRA_CONFIG="sfsreportreservedperiod=1000000" \
	CHUNKSERVER_EXTRA_CONFIG="MASTER_TIMEOUT = 2|MASTER_RECONNECTION_DELAY = 1|`
		`MAGIC_DEBUG_LOG=${TEMP_DIR}/chunkserver.log|LOG_FLUSH_ON=DEBUG" \
	setup_local_empty_saunafs info

source "${SOURCE_DIR}/tests/tools/session_release_helpers.sh"

session_release_hold_deleted_files "${info[mount0]}/held" "${files}" \
	"${files_per_holder}" subdirectories
setup_start=$(date +%s)
session_release_wait_for_holders '20 minutes'
echo "Session expiry: ${files} files open and deleted in $(($(date +%s) - setup_start)) s"
assert_equals "${files}" "$(session_release_reserved_files)"

samples="${TEMP_DIR}/samples"
session_release_start_timed_sampling "${samples}"

# A killed mount leaves its session to expire after SESSION_SUSTAIN_TIME.
mount0=$(session_release_mount_process 0)
assert_matches '^[0-9]+$' "${mount0}"
assert_success kill -KILL "${mount0}"
assert_success saunafs_fusermount -u -z "${info[mount0]}"
session_release_wait_for_sampling

session_release_report_timed_samples 'Session expiry' "${files}" "${samples}"
session_release_expect_timed_responsiveness
session_release_cleanup

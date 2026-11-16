timeout_set 5 minutes

# Releasing the open files of a deleted or expired session must not stop the master from serving
# its peers. Both sessions hold files deleted while open, so every released file purges one
# reserved file, and the reserved-file count shows how far the release went. Seeing a count
# between its start and its end proves that the master answered a client during the release.
files=${SESSION_RELEASE_FILES:-20000}

# Set sfsreportreservedperiod beyond the test deadline. A report sent before
# the mount records a create reply can drop the new file's open reference.
CHUNKSERVERS=1 \
	MOUNTS=2 \
	USE_RAMDISK=YES \
	MASTER_EXTRA_CONFIG="SESSION_SUSTAIN_TIME=60" \
	MOUNT_EXTRA_CONFIG="sfsreportreservedperiod=1800" \
	CHUNKSERVER_EXTRA_CONFIG="MASTER_TIMEOUT = 2|MAGIC_DEBUG_LOG=${TEMP_DIR}/chunkserver.log|`
		`LOG_FLUSH_ON=DEBUG" \
	setup_local_empty_saunafs info

source "${SOURCE_DIR}/tests/tools/session_release_helpers.sh"

# Files one process holds open, below the open-file limit of the test processes.
files_per_holder=5000

session_release_hold_deleted_files "${info[mount0]}/held0" "${files}" "${files_per_holder}"
session_release_hold_deleted_files "${info[mount1]}/held1" "${files}" "${files_per_holder}"
session_release_wait_for_holders '3 minutes'
assert_equals "$((2 * files))" "$(session_release_reserved_files)"

# delete-session: a stopped mount cannot reconnect, so only the release frees its files.
session=$(session_release_session_id 0)
assert_matches '^[0-9]+$' "${session}"
mount0=$(session_release_mount_process 0)
assert_matches '^[0-9]+$' "${mount0}"
assert_success kill -STOP "${mount0}"
session_release_start_sampling "${files}" "${TEMP_DIR}/delete-samples"
assert_success leil-admin delete-session localhost "${info[matocl]}" "${session}"
session_release_expect_answer_during_release "${files}" "$((2 * files))" "${TEMP_DIR}/delete-samples"
assert_success kill -KILL "${mount0}"
assert_success saunafs_fusermount -u -z "${info[mount0]}"

# Expiry: a killed mount leaves its session to expire after SESSION_SUSTAIN_TIME.
mount1=$(session_release_mount_process 1)
assert_matches '^[0-9]+$' "${mount1}"
session_release_start_sampling 0 "${TEMP_DIR}/expiry-samples"
assert_success kill -KILL "${mount1}"
assert_success saunafs_fusermount -u -z "${info[mount1]}"
session_release_expect_answer_during_release 0 "${files}" "${TEMP_DIR}/expiry-samples"

# The chunkserver never lost the master: it registered once, at start.
assert_equals 1 "$(grep -c 'MasterConn: registered to MDS' "${TEMP_DIR}/chunkserver.log")"
session_release_cleanup

timeout_set 2 minutes

# Only the file backend uses this checkpoint and changelog for failover.
if [[ "${METADATA_BACKEND:-FILE}" != FILE ]]; then test_end; fi
CHUNKSERVERS=1 \
	MASTERSERVERS=2 \
	AUTO_SHADOW_MASTER=NO \
	MOUNTS=2 \
	USE_RAMDISK=YES \
	MASTER_1_EXTRA_CONFIG="AUTO_RECOVERY=1" \
	MOUNT_EXTRA_CONFIG="enablefilelocks=1|sfscachemode=NEVER" \
	setup_local_empty_saunafs info

source "${SOURCE_DIR}/tests/tools/session_lock_helpers.sh"
source "${SOURCE_DIR}/tests/tools/session_release_helpers.sh"

cleanup_retirement_test() {
	if [[ -n "${paused_mount:-}" ]]; then kill -CONT "${paused_mount}" 2>/dev/null || true; fi
	cleanup_session_locks
}
test_error_cleanup() { cleanup_retirement_test; }
trap cleanup_retirement_test EXIT

# A holds two exclusive locks, and B waits for the lock on the first inode.
for name in contended remaining; do
	assert_success touch "${info[mount0]}/${name}"
	lock_inodes[${name}]=$(stat -c %i "${info[mount0]}/${name}")
	lock_kinds[${name}]=flock
	session_lock_start_process "${name}" 0 w holder
	assert_eventually "grep -q '^write lock:' '${TEMP_DIR}/${name}-holder.log'"
done
contended_inode="${lock_inodes[contended]}"
remaining_inode="${lock_inodes[remaining]}"
assert_success test "${contended_inode}" -lt "${remaining_inode}"

session_lock_start_process contended 1 w waiter
assert_eventually_prints 1 "session_lock_count ${info[matocl]} flock pending ${contended_inode}"
owner_id=$(session_release_session_id 0)
waiter_id=$(session_release_session_id 1)
assert_matches '^[0-9]+$' "${owner_id}"
assert_matches '^[0-9]+$' "${waiter_id}"
assert_not_equal "${owner_id}" "${waiter_id}"

# Preserve the pre-release metadata and session table on the failover server.
primary_data="${info[master0_data_path]}"
replacement_data="${info[master1_data_path]}"
assert_success saunafs_admin_master save-metadata "--timeout=$(timeout_rescale_seconds 30)"
assert_success cp "${primary_data}/metadata.sfs" "${primary_data}/sessions.sfs" "${replacement_data}/"

# Keep A's lock processes and mount alive, but prevent A from reconnecting to the old master.
mount_pid=$(session_release_mount_process 0)
assert_matches '^[0-9]+$' "${mount_pid}"
assert_success kill -STOP "${mount_pid}"
paused_mount="${mount_pid}"
cleanup_boundary=$(get_changes "${primary_data}" | tail -1 | cut -d: -f1)
assert_matches '^[0-9]+$' "${cleanup_boundary}"
delete_reply=$(leil-admin delete-session localhost "${info[matocl]}" "${owner_id}")
assert_equals "Session deleted successfully" "${delete_reply}"
assert_eventually "grep -q '^write lock:' '${TEMP_DIR}/contended-waiter.log'"

# Replay through the first release only, which gives B the lock and leaves A's second lock intact.
get_changes "${primary_data}" > "${TEMP_DIR}/primary-changelog"
assert_success awk -F: -v boundary="${cleanup_boundary}" \
	-v inode="${contended_inode}" -v session="${owner_id}" '
	{print}
	$1 > boundary && index($0, "|RELEASE(" inode "," session ")") {found=1; exit}
	END {exit !found}
' "${TEMP_DIR}/primary-changelog" > "${replacement_data}/changelog.sfs"

# The primary still owns the ports. Recovery checkpoints the partial replay before
# binding the sockets, then startup fails before the session sweep can run.
release_version=$(tail -n 1 "${replacement_data}/changelog.sfs" | cut -d: -f1)
saunafs_make_conf_for_master 1
assert_failure saunafs_master_daemon start
# Failed startup returns before the child finishes its shutdown metadata dump.
assert_eventually "! saunafs_master_daemon isalive"
assert_file_not_exists "${replacement_data}/metadata.sfs.tmp"
assert_equals "$((release_version + 1))" "$(metadata_get_version "${replacement_data}/metadata.sfs")"

session_lock_image "${replacement_data}/metadata.sfs" "${TEMP_DIR}/replayed-locks"
assert_equals "${waiter_id}" "$(awk -v inode="${contended_inode}" \
	'$1 == "flock" && $2 == "active" && $3 == inode {print $6}' "${TEMP_DIR}/replayed-locks")"
assert_equals "${owner_id}" "$(awk -v inode="${remaining_inode}" \
	'$1 == "flock" && $2 == "active" && $3 == inode {print $6}' "${TEMP_DIR}/replayed-locks")"

# The second startup reads the new checkpoint and the persisted session table.
assert_success saunafs_master_n 0 kill
assert_success saunafs_master_daemon start
assert_eventually_prints "${waiter_id}" 'session_release_session_id 1'

# A first attempts to reconnect after the second startup.
assert_success kill -CONT "${paused_mount}"
paused_mount=
assert_eventually '[[ "$(session_release_session_id 0)" =~ ^[0-9]+$ ]]'
assert_equals "${waiter_id}" "$(session_lock_rows "${info[matocl]}" flock active \
	"${contended_inode}" | awk '{print $3}')"
assert_not_equal "${owner_id}" "$(session_release_session_id 0)"

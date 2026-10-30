timeout_set 2 minutes

sweep_milliseconds=$((1000 * $(timeout_rescale_seconds 1)))
CHUNKSERVERS=1 \
	MASTERSERVERS=2 \
	MOUNTS=2 \
	USE_RAMDISK=YES \
	MASTER_EXTRA_CONFIG="EMPTY_RESERVED_FILES_PERIOD_MSECONDS=${sweep_milliseconds}" \
	MOUNT_EXTRA_CONFIG="enablefilelocks=1|sfscachemode=NEVER" \
	setup_local_empty_saunafs info

source "${SOURCE_DIR}/tests/tools/session_lock_helpers.sh"
assert_success saunafs_master_n 1 start

# Keep files linked until both requests are stored; the sweep may run at any time after unlink.
for kind in flock posix; do
	name="swept-${kind}"
	file="${info[mount0]}/${name}"
	assert_success touch "${file}"
	assert_success leil settrashtime 0 "${file}"
	lock_inodes[${name}]=$(stat -c %i "${file}")
	lock_kinds[${name}]="${kind}"
	lock_cases+=("${name}")
	session_lock_start_process "${name}" 0 w holder
	assert_eventually_prints 1 \
		"session_lock_count ${info[matocl]} ${kind} active ${lock_inodes[${name}]}"
	session_lock_start_process "${name}" 1 w waiter
	assert_eventually_prints 1 \
		"session_lock_count ${info[matocl]} ${kind} pending ${lock_inodes[${name}]}"
done
for name in "${lock_cases[@]}"; do assert_success rm "${info[mount0]}/${name}"; done

# A zero reserved count witnesses removal of both unlinked inodes while their FDs remain open.
swept_reserved_files() {
	local output
	output=$(leil-admin info localhost "${info[matocl]}") || return
	awk '/^Reserved files:/ {print $3}' <<< "${output}"
}
assert_eventually_prints 0 swept_reserved_files
for name in "${lock_cases[@]}"; do session_lock_assert_state "${name}" 1 1; done
session_lock_shadow_matches

departing_session=$(session_lock_rows "${info[matocl]}" flock active \
	"${lock_inodes[swept-flock]}" | awk '{print $3}')
assert_matches '^[0-9]+$' "${departing_session}"
cleanup_version=$(get_changes "${info[master0_data_path]}" | tail -1 | cut -d: -f1)
assert_matches '^[0-9]+$' "${cleanup_version}"
assert_success leil-admin delete-session localhost "${info[matocl]}" "${departing_session}"

# Missing-inode cleanup must keep the released replies without logging an unreplayable FLCK.
for name in "${lock_cases[@]}"; do
	session_lock_assert_state "${name}" 0 0
	assert_eventually "grep -q '^write lock:' '${TEMP_DIR}/${name}-waiter.log'"
	assert_failure grep -Eq 'failed:|Failed to' "${TEMP_DIR}/${name}-waiter.log"
done
get_changes "${info[master0_data_path]}" > "${TEMP_DIR}/swept-cleanup-changelog"
assert_failure awk -F: -v boundary="${cleanup_version}" \
	'$1 > boundary && index($0, "|FLCK(") {found=1} END {exit !found}' \
	"${TEMP_DIR}/swept-cleanup-changelog"
session_lock_shadow_matches
cleanup_session_locks

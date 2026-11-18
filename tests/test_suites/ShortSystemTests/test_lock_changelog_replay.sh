# Verify rejected nonblocking requests never become waiters during changelog replay.
# For flock and POSIX locks, hold an exclusive lock and reject shared/exclusive
# contenders. Keep both descriptors open while comparing Master, Shadow and
# metarestore lock state, so later releases cannot mask phantom waiters. Then
# close the contender, unlock the holder, and verify empty lock state with no
# Shadow replay or resynchronization errors.

start_rejected_nonblocking_request() {
	local name="${1}" kind="${2}" mode="${3}"
	local ready="${TEMP_DIR}/${name}.ready" finish="${TEMP_DIR}/${name}.finish"

	# Signal rejection, then keep the descriptor open until the parent permits its close.
	python3 - "${info[mount1]}/${name}" "${kind}" "${mode}" "${ready}" "${finish}" <<'PY' &
import errno
import fcntl
from pathlib import Path
import sys
import time

filename, kind, mode, ready, finish = sys.argv[1:]
operation = fcntl.LOCK_SH if mode == 'r' else fcntl.LOCK_EX
acquire = fcntl.flock if kind == 'flock' else fcntl.lockf
with open(filename, 'r+') as descriptor:
    try:
        acquire(descriptor, operation | fcntl.LOCK_NB)
    except OSError as error:
        # LS-1308: the current client maps WAITING to EINVAL. Remove this allowance
        # once https://linear.app/leil/issue/LS-1308 fixes the conflict errno.
        if error.errno not in (errno.EAGAIN, errno.EACCES, errno.EINVAL):
            raise
    else:
        raise AssertionError('conflicting nonblocking lock unexpectedly succeeded')
    Path(ready).touch()
    while not Path(finish).exists():
        time.sleep(0.02)
PY
	lock_started_process=$!
	lock_processes[${lock_started_process}]=1
	assert_eventually "test -f '${ready}' || ! kill -0 ${lock_started_process} 2>/dev/null"
	assert_success test -f "${ready}"
}

assert_metarestore_matches_master() {
	assert_success test -s "${TEMP_DIR}/master-locks"
	get_changes "${info[master0_data_path]}" |
		awk -F: -v first="${checkpoint_version}" '$1 >= first' \
		> "${TEMP_DIR}/lock-changelog"
	assert_success sfsmetarestore -m "${TEMP_DIR}/checkpoint.sfs" \
		-o "${TEMP_DIR}/restored.sfs" "${TEMP_DIR}/lock-changelog" \
		> "${TEMP_DIR}/metarestore.log" 2>&1
	session_lock_image "${TEMP_DIR}/restored.sfs" "${TEMP_DIR}/restored-locks"
	assert_files_equal "${TEMP_DIR}/master-locks" "${TEMP_DIR}/restored-locks"
}

wait_for_lock_process() {
	local process="${1}" status=0
	wait "${process}" || status=$?
	assert_equals 0 "${status}"
	unset "lock_processes[${process}]"
}

check_nonblocking_lock_replay() {
	local kind="${1}" mode="${2}"
	local name="nonblocking-${kind}-${mode}" holder contender
	assert_success touch "${info[mount0]}/${name}"
	lock_inodes[${name}]=$(stat -c %i "${info[mount0]}/${name}")
	lock_kinds[${name}]="${kind}"
	session_lock_start_process "${name}" 0 w holder
	holder=${lock_started_process}
	assert_eventually_prints 1 \
		"session_lock_count ${info[matocl]} ${kind} active ${lock_inodes[${name}]}"
	start_rejected_nonblocking_request "${name}" "${kind}" "${mode}"
	contender=${lock_started_process}

	# Compare before either descriptor closes, so cleanup cannot hide phantom waiters.
	session_lock_assert_state "${name}" 1 0
	session_lock_shadow_matches
	assert_metarestore_matches_master

	assert_success touch "${TEMP_DIR}/${name}.finish"
	wait_for_lock_process "${contender}"
	assert_success kill -USR1 "${holder}"
	wait_for_lock_process "${holder}"
	assert_eventually_prints 0 \
		"session_lock_count ${info[matocl]} ${kind} active ${lock_inodes[${name}]}"
	session_lock_assert_state "${name}" 0 0
	session_lock_shadow_matches
}

timeout_set 2 minutes

CHUNKSERVERS=1 \
	MASTERSERVERS=2 \
	MOUNTS=2 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="enablefilelocks=1|sfscachemode=NEVER" \
	setup_local_empty_saunafs info

source "${SOURCE_DIR}/tests/tools/session_lock_helpers.sh"
assert_success saunafs_master_n 1 start
assert_eventually 'saunafs_shadow_synchronized 1'
syslog_next_line=$(($(wc -l < /var/log/syslog) + 1))

assert_success saunafs_admin_master save-metadata "--timeout=$(timeout_rescale_seconds 30)"
assert_success cp "${info[master0_data_path]}/metadata.sfs" "${TEMP_DIR}/checkpoint.sfs"

# The metadata version identifies the next changelog entry to apply.
checkpoint_version=$(metadata_get_version "${TEMP_DIR}/checkpoint.sfs")
assert_matches '^[1-9][0-9]*$' "${checkpoint_version}"

for kind in flock posix; do
	for mode in r w; do
		check_nonblocking_lock_replay "${kind}" "${mode}"
	done
done

replay_log=$(tail -n +"${syslog_next_line}" /var/log/syslog)
assert_failure grep -Eq \
	'malformed changelog sent|some changes lost|Waiting for master to produce up-to-date metadata image' \
	<<< "${replay_log}"

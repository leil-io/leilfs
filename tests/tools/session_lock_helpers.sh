# Lock processes stay alive until stored owners and successful replies have both been checked.
declare -A lock_inodes lock_kinds lock_waiters lock_logs lock_processes
lock_cases=()
lock_orphans=()

cleanup_session_locks() {
	local process
	for process in "${!lock_processes[@]}"; do
		kill -KILL "${process}" 2>/dev/null || true
	done
	for process in "${!lock_processes[@]}"; do
		wait "${process}" 2>/dev/null || true
	done
	lock_processes=()
}
test_error_cleanup() { cleanup_session_locks; }
trap cleanup_session_locks EXIT

session_lock_rows() {
	local port="${1}" kind="${2}" state="${3}" inode="${4:-0}" output
	output=$(leil-admin manage-locks --porcelain "--${state}" "--inode=${inode}" \
		localhost "${port}" list "${kind}" <<< "${info[admin_password]}") || return
	awk '$1 ~ /^[0-9]+$/ {print}' <<< "${output}" | LC_ALL=C sort
}

session_lock_count() {
	local output
	output=$(session_lock_rows "${@}") || return
	awk 'NF {count++} END {print count+0}' <<< "${output}"
}

session_lock_dump() {
	local server="${1}" destination="${2}"
	if [[ "${server}" == "$(saunafs_current_master_id)" ]]; then
		assert_success saunafs_admin_master save-metadata \
			"--timeout=$(timeout_rescale_seconds 30)"
	else
		assert_success saunafs_admin_shadow "${server}" save-metadata \
			"--timeout=$(timeout_rescale_seconds 30)"
	fi
	assert_success python3 "${SOURCE_DIR}/tests/tools/session_lock_image.py" \
		"${info[master${server}_data_path]}/metadata.sfs" > "${destination}"
}

session_lock_shadow_matches() {
	# Shadow has no lock-list RPC; completed dumps expose the full active and pending state.
	assert_eventually 'saunafs_shadow_synchronized 1'
	session_lock_dump 0 "${TEMP_DIR}/master-locks"
	session_lock_dump 1 "${TEMP_DIR}/shadow-locks"
	assert_files_equal "${TEMP_DIR}/master-locks" "${TEMP_DIR}/shadow-locks"
}

session_lock_start_process() {
	local name="${1}" mount="${2}" mode="${3}" role="${4}" command=flockcmd
	if [[ "${lock_kinds[${name}]}" == posix ]]; then command=posixlockcmd; fi
	local logfile="${TEMP_DIR}/${name}-${role}.log"
	"${command}" "${info[mount${mount}]}/${name}" "${mode}" > "${logfile}" 2>&1 &
	lock_started_process=$!
	lock_processes[${lock_started_process}]=1
}

session_lock_start_case() {
	local name="${1}" kind="${2}" holder="${3}" orphan="${4:-0}" owner mount
	assert_success touch "${info[mount${holder}]}/${name}"
	assert_success leil settrashtime 0 "${info[mount${holder}]}/${name}"
	lock_inodes[${name}]=$(stat -c %i "${info[mount${holder}]}/${name}")
	lock_kinds[${name}]="${kind}"

	# Distinct owners in one session share the range; a third has only a pending request.
	for owner in 1 2; do
		session_lock_start_process "${name}" "${holder}" r "holder${owner}"
	done
	assert_eventually_prints 2 \
		"session_lock_count ${info[matocl]} ${kind} active ${lock_inodes[${name}]}"
	if [[ "${orphan}" == 1 ]]; then
		lock_orphans+=("${name}")
	else
		lock_cases+=("${name}")
		session_lock_start_process "${name}" "${holder}" w departing
		assert_eventually_prints 1 \
			"session_lock_count ${info[matocl]} ${kind} pending ${lock_inodes[${name}]}"
		for mount in 2 3; do
			session_lock_start_process "${name}" "${mount}" w "waiter${mount}"
			lock_waiters[${name},${mount}]="${lock_started_process}"
			lock_logs[${name},${mount}]="${TEMP_DIR}/${name}-waiter${mount}.log"
		done
		assert_eventually_prints 3 \
			"session_lock_count ${info[matocl]} ${kind} pending ${lock_inodes[${name}]}"
	fi

	# Cleanup must still find the inode while it is reserved only by open file references.
	assert_success rm "${info[mount${holder}]}/${name}"
}

session_lock_prepare() {
	local kind
	for kind in flock posix; do
		session_lock_start_case "delete-${kind}" "${kind}" 0
		session_lock_start_case "timeout-${kind}" "${kind}" 1
		session_lock_start_case "orphan-${kind}" "${kind}" 0 1
	done
	lock_delete_session=$(session_lock_rows "${info[matocl]}" flock active \
		"${lock_inodes[delete-flock]}" | awk '{print $3}' | sort -u)
	lock_timeout_session=$(session_lock_rows "${info[matocl]}" flock active \
		"${lock_inodes[timeout-flock]}" | awk '{print $3}' | sort -u)
	assert_matches '^[0-9]+$' "${lock_delete_session}"
	assert_matches '^[0-9]+$' "${lock_timeout_session}"
	assert_not_equal "${lock_delete_session}" "${lock_timeout_session}"
}

session_lock_assert_state() {
	local name="${1}" active="${2}" pending="${3}"
	assert_equals "${active}" "$(session_lock_count "${info[matocl]}" \
		"${lock_kinds[${name}]}" active "${lock_inodes[${name}]}")"
	assert_equals "${pending}" "$(session_lock_count "${info[matocl]}" \
		"${lock_kinds[${name}]}" pending "${lock_inodes[${name}]}")"
}

session_lock_session_locks_gone() {
	local kind state rows
	for kind in flock posix; do
		for state in active pending; do
			rows=$(session_lock_rows "${info[matocl]}" "${kind}" "${state}") || return
			if awk -v session="${1}" '$3 == session {found=1} END {exit !found}' <<< "${rows}"; then
				return 1
			fi
		done
	done
	return 0
}

session_lock_release_holders() {
	assert_success leil-admin delete-session localhost "${info[matocl]}" "${lock_delete_session}"
	local name
	for name in "${lock_orphans[@]}"; do session_lock_assert_state "${name}" 0 0; done

	# A disconnected session vanishes from the live list before its stored locks are released.
	local mount_command="${info[mnt1_command]}" process
	process=$(pgrep -f -u "$(id -u)" \
		"\\<${mount_command##*/}\\>.*${info[mount1]}")
	assert_matches '^[0-9]+$' "${process}"
	assert_success kill -KILL "${process}"
	assert_eventually "! kill -0 ${process} 2>/dev/null"
	assert_success saunafs_fusermount -u -z "${info[mount1]}"
	assert_eventually "session_lock_session_locks_gone ${lock_timeout_session}" '2 minutes'
}

session_lock_grants() {
	local name="${1}"
	awk '/^write lock:/ {count++} END {print count+0}' \
		"${lock_logs[${name},2]}" "${lock_logs[${name},3]}"
}

session_lock_check_release() {
	local name state rows
	for name in "${lock_cases[@]}"; do
		# Check storage before waiting for replies, so missing pending-owner cleanup fails here.
		session_lock_assert_state "${name}" 1 1
		for state in active pending; do
			rows=$(session_lock_rows "${info[matocl]}" "${lock_kinds[${name}]}" \
				"${state}" "${lock_inodes[${name}]}")
			assert_failure awk -v deleted="${lock_delete_session}" \
				-v timedout="${lock_timeout_session}" \
				'$3 == deleted || $3 == timedout {found=1} END {exit !found}' <<< "${rows}"
		done
		assert_eventually_prints 1 "session_lock_grants ${name}"
		assert_failure grep -Eq 'failed:|Failed to' \
			"${lock_logs[${name},2]}" "${lock_logs[${name},3]}"
	done
}

session_lock_unlock_first() {
	local name="${1}" mount=3 process first_session second_session status=0
	if grep -q '^write lock:' "${lock_logs[${name},2]}"; then mount=2; fi
	process="${lock_waiters[${name},${mount}]}"
	first_session=$(session_lock_rows "${info[matocl]}" "${lock_kinds[${name}]}" active \
		"${lock_inodes[${name}]}" | awk '{print $3}')
	assert_success kill -USR1 "${process}"
	assert_eventually_prints 2 "session_lock_grants ${name}"
	session_lock_assert_state "${name}" 1 0
	second_session=$(session_lock_rows "${info[matocl]}" "${lock_kinds[${name}]}" active \
		"${lock_inodes[${name}]}" | awk '{print $3}')
	assert_not_equal "${first_session}" "${second_session}"
	assert_failure grep -Eq 'failed:|Failed to' \
		"${lock_logs[${name},2]}" "${lock_logs[${name},3]}"
	assert_eventually "! kill -0 ${process} 2>/dev/null"
	wait "${process}" || status=$?
	unset 'lock_processes['"${process}"']'
	assert_equals 0 "${status}"
}

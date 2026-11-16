# Shared file holders, sampling, and diagnostics for session-release responsiveness tests.
session_release_holders=()
session_release_sampler=""

session_release_cleanup() {
	local process
	if [[ -n "${session_release_sampler}" ]]; then
		kill -KILL "${session_release_sampler}" 2>/dev/null || true
		wait "${session_release_sampler}" 2>/dev/null || true
		session_release_sampler=""
	fi
	for process in "${session_release_holders[@]}"; do
		kill -KILL "${process}" 2>/dev/null || true
	done
	for process in "${session_release_holders[@]}"; do
		wait "${process}" 2>/dev/null || true
	done
	session_release_holders=()
}
test_error_cleanup() { session_release_cleanup; }
trap session_release_cleanup EXIT

# Flat directories use disjoint filename ranges; manual tests use a directory per holder.
session_release_hold_deleted_files() {
	local directory="${1}" files="${2}" per_holder="${3}" layout="${4:-flat}"
	local first count holder_directory holder_first
	local -a options
	assert_success mkdir "${directory}"
	assert_success leil settrashtime 0 "${directory}"
	for ((first = 0; first < files; first += per_holder)); do
		count=$((files - first < per_holder ? files - first : per_holder))
		holder_directory="${directory}"
		holder_first="${first}"
		options=()
		if [[ "${layout}" == subdirectories ]]; then
			holder_directory="${directory}/${first}"
			holder_first=0
			options+=(--create-directory)
		fi
		python3 "${SOURCE_DIR}/tests/tools/session_release_hold_files.py" \
			"${holder_directory}" "${holder_first}" "${count}" "${options[@]}" \
			>> "${TEMP_DIR}/holders.log" 2>&1 &
		session_release_holders+=($!)
	done
}

session_release_wait_for_holders() {
	assert_eventually_prints "${#session_release_holders[@]}" \
		"grep -c ready ${TEMP_DIR}/holders.log" "${1}"
}

session_release_reserved_files() {
	local output reserved
	output=$(saunafs_admin_master_no_password info) || return
	reserved=$(awk '{print $8}' <<< "${output}")
	[[ "${reserved}" =~ ^[0-9]+$ ]] || return 1
	echo "${reserved}"
}

session_release_mount_process() {
	local command="${info[mnt${1}_command]}"
	pgrep -f -u "$(id -u)" "\\<${command##*/}\\>.*${info[mount${1}]}"
}

session_release_session_id() {
	leil-admin list-mounts localhost "${info[matocl]}" --porcelain | \
		awk -v mount="${info[mount${1}]}" '$3 == mount {print $1}'
}

# Sample before triggering cleanup so even a fast release can be observed in progress.
session_release_start_sampling() {
	local until="${1}" samples="${2}"
	(
		trap - EXIT
		while true; do
			reserved=$(session_release_reserved_files) || exit 1
			echo "${reserved}" >> "${samples}"
			(( reserved > until )) || exit 0
		done
	) &
	session_release_sampler=$!
	assert_eventually "test -s ${samples}"
}

# Manual tests also record request latency and retain unanswered requests as -1.
session_release_start_timed_sampling() {
	local samples="${1}"
	(
		trap - EXIT
		while true; do
			start=$(date +%s.%N)
			if ! reserved=$(session_release_reserved_files) || [[ -z "${reserved}" ]]; then
				reserved=-1
			fi
			echo "${reserved} $(awk -v start="${start}" -v end="$(date +%s.%N)" \
				'BEGIN {printf "%.3f", end - start}')" >> "${samples}"
			[[ "${reserved}" == 0 ]] && exit 0
		done
	) &
	session_release_sampler=$!
	assert_eventually "test -s ${samples}"
}

session_release_wait_for_sampling() {
	wait "${session_release_sampler}"
	session_release_sampler=""
}

# An intermediate count proves the master answered while cleanup was still in progress.
session_release_expect_answer_during_release() {
	local low="${1}" high="${2}" samples="${3}"
	session_release_wait_for_sampling
	expect_less_than 0 "$(awk -v low="${low}" -v high="${high}" \
		'$1 > low && $1 < high {count++} END {print count+0}' "${samples}")"
}

session_release_report_timed_samples() {
	local label="${1}" files="${2}" samples="${3}"
	# Allow MASTER_TIMEOUT plus MASTER_RECONNECTION_DELAY for any new registration attempt.
	sleep 5
	session_release_answered=$(awk -v files="${files}" '$1 > 0 && $1 < files' "${samples}" | wc -l)
	session_release_slowest=$(awk '$2 > slowest {slowest = $2} END {print slowest}' "${samples}")
	session_release_failed=$(awk '$1 == -1' "${samples}" | wc -l)
	session_release_registrations=$(grep -c 'MasterConn: registering to MDS' \
		"${TEMP_DIR}/chunkserver.log")
	echo "${label}: answers during the release: ${session_release_answered}," \
		"slowest answer: ${session_release_slowest} s, failed requests: ${session_release_failed}," \
		"chunkserver registration attempts: ${session_release_registrations}"
	awk '{print $1}' "${samples}" | uniq -c | tail -n 40
}

session_release_expect_timed_responsiveness() {
	expect_less_than 0 "${session_release_answered}"
	expect_equals 0 "${session_release_failed}"
	expect_equals 1 "${session_release_registrations}"
}

# Retain the status for the caller to check after cleanup and diagnostic reporting.
session_release_timed_delete() {
	local session="${1}" start output seconds
	start=$(date +%s.%N)
	session_release_delete_status=0
	output=$(leil-admin delete-session localhost "${info[matocl]}" "${session}" 2>&1) ||
		session_release_delete_status=$?
	seconds=$(awk -v start="${start}" -v end="$(date +%s.%N)" \
		'BEGIN {printf "%.3f", end - start}')
	echo "Session deletion: delete-session took ${seconds} s," \
		"exit status ${session_release_delete_status}: ${output}"
}

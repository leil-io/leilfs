timeout_killer_thread() {
	# Python3 is used here to perform floating-point multiplication in timeout computation
	assert_program_installed python3

	local machine_environment_multiplier=1
	if [[ ! -z "${SAUNAFS_TEST_TIMEOUT_MULTIPLIER:-}" ]]; then
		machine_environment_multiplier="$SAUNAFS_TEST_TIMEOUT_MULTIPLIER"
	fi

	while ! test_frozen; do
		sleep 1
		if test_frozen; then
			return
		fi

		local multiplier=$(cat "$test_timeout_multiplier_file")
		local begin_ts=$(cat "$test_timeout_begin_ts_file") || true
		local value_string=$(cat "$test_timeout_value_file") || true
		local value=$(($(date +%s -d "$value_string") - $(date +%s)))
		local now_ts=$(date +%s)

		# Increase test time in Windows environment
		if is_windows_system; then
			multiplier=$((5 * $multiplier))
		fi

		if [[ -z $begin_ts || -z $value_string || -z $multiplier ]]; then
			# A race with timeout_set occurred (it truncates the endTS file and then writes it)
			# or a race with test_cleanup (test_timeout_end_ts_file has just been removed)
			continue
		fi

		local total_timeout=$(($value * $multiplier * $machine_environment_multiplier))
		local end_ts=$(($begin_ts + $total_timeout))

		if (( now_ts >= end_ts )); then
			local format=$(cat <<-END
				Test timed out (Timeout: %d seconds)
				   Timeout components: TEST TIMEOUT VALUE | MACHINE'S MULTIPLIER | FRAMEWORK'S MULTIPLIER (e.g. for running under Valgrind)
				         [in seconds]  %18s | %20s | %22s
				 timeout_set argument: %18s |
			END
			)

			local test_timeout_message=$( printf "$format" \
				"$total_timeout" \
				"$value" "$machine_environment_multiplier" "$multiplier" \
				"$value_string" )

			test_add_failure "$test_timeout_message"
			test_freeze_result
			# Stop LeilFS and FDB before the sweep. fdb_cluster_started is set
			# after this subshell forks, so cleanup_fdb_cluster runs
			# unconditionally (safe no-op without a cluster). killall stays
			# last: it kills this shell too.
			terminate_fs_processes
			cleanup_fdb_cluster
			killall -9 -u $(whoami)
		fi
	done
}

timeout_init() {
	test_timeout_begin_ts_file="$TEMP_DIR/$(unique_file)_timeout_beginTS.txt"
	test_timeout_multiplier_file="$TEMP_DIR/$(unique_file)_timeout_multiplier.txt"
	test_timeout_value_file="$TEMP_DIR/$(unique_file)_timeout_value.txt"

	# default timeout values
	timeout_set "30 seconds"
	timeout_set_multiplier 1

	# Parentheses below are needed to make 'wait' command work in tests.
	# They make the killer thread to be a job owned by a temporary subshell, not ours
	( timeout_killer_thread & )
}

timeout_set() {
	echo "$*" > "$test_timeout_value_file"
	date +%s > "$test_timeout_begin_ts_file"
}

timeout_set_multiplier() {
	echo "$1" > "$test_timeout_multiplier_file"
}

timeout_get_multiplier() {
	cat "$test_timeout_multiplier_file"
}

# takes as parameter timeout (e.g. '1 minute') and rescales it by multiplier
# prints result in seconds to standard output (e.g. '900 seconds')
timeout_rescale() {
	local multiplier=$(timeout_get_multiplier)
	local value=$(($(date +%s -d "$1") - $(date +%s)))
	echo $((value * multiplier)) seconds
}

# takes as parameter timeout in seconds (e.g. '60') and rescales it by multiplier
# prints result to standard output (e.g. '900')
timeout_rescale_seconds() {
	local multiplier=$(timeout_get_multiplier)
	echo $(($1 * multiplier))
}

# Not enabled yet.
valgrind_enabled_=
valgrind_tool_="memcheck"

# A script which runs valgrind. Its file name will be generated in valgrind_enable
valgrind_script_=

valgrind_enabled() {
	test -z $valgrind_enabled_ && return 1 || return 0
}

getMemcheckOptions() {
	local options=""

	options+=" --leak-check=full"

	# New ( >= 3.9) versions of valgrind support some nice heuristics which remove
	# a couple of false positives (eg. leak reports when there is a reachable std::string).
	# Use the heuristics if available.
	if valgrind --leak-check-heuristics=all true &>/dev/null; then
		options+=" --leak-check-heuristics=all"
	fi

	echo $options
}

getHelgrindOptions() {
	local options=""

	options+=" --ignore-thread-creation=no"
	options+=" --track-lockorders=yes"
	options+=" --read-var-info=no"
	# Possible values: full, approx, none
	options+=" --history-level=full"
	options+=" --delta-stacktrace=yes"
	options+=" --free-is-write=yes"
	options+=" --cmp-race-err-addrs=yes"
	options+=" --check-stack-refs=yes"

	echo $options
}

# Enables valgrind, can be called at the beginning of a test case.
valgrind_enable() {
	if [[ $# -gt 1 ]]; then
		echo " --- Error: valgrind_enable accepts at most one argument --- "
		echo " --- valgrind won't be enabled --- "
		return 1
	fi

	assert_program_installed valgrind
	valgrind_version=$(valgrind --version | cut -d'-' -f 2)
	if ! version_compare_gte "$valgrind_version" "3.15.0" ; then
		echo " --- Error: Minimum valgrind version supported is 3.15.0 but yours is $valgrind_version --- "
		echo " --- valgrind won't be enabled --- "
		return 1
	fi

	# If one argument was provided, then it must be memcheck or helgrind
	if [[ $# -eq 1 ]]; then
		if [[ $1 == "memcheck" || $1 == "helgrind" ]]; then
			valgrind_tool_="$1"
		else
			echo " --- Error: valgrind tool must be memcheck or helgrind --- "
			echo " --- valgrind won't be enabled --- "
			return 1
		fi
	fi

	if [[ -z $valgrind_enabled_ ]]; then
		valgrind_enabled_=1

		valgrind_command="valgrind -q --tool=${valgrind_tool_} ";

		if [[ $valgrind_tool_ == "memcheck" ]]; then
			valgrind_command+="$(getMemcheckOptions)"
		else
			valgrind_command+="$(getHelgrindOptions)"
		fi

		# Suppressions files for known false positives
		valgrind_command+=" --suppressions=$SOURCE_DIR/tests/tools/valgrind-${valgrind_tool_}.supp"

		# Valgrind error messages will be written here.
		valgrind_command+=" --log-file=${ERROR_DIR}/valgrind__\${1}_%p.log"

		# Valgrind errors will generate suppressions:
		valgrind_command+=" --gen-suppressions=all"

		# Valgrind will show filepaths with module subdirectories on errors:
		valgrind_command+=" --fullpath-after=src/"

		# Create a script which will run processes on valgrind. This to make it possible
		# to modify this script to stop spawning new valgrind processes in valgrind_terminate.
		valgrind_script_="$TEMP_DIR/$(unique_file)_valgrind.sh"
		echo -e "#!/usr/bin/env bash\nexec $valgrind_command \"\$@\"" > "$valgrind_script_"
		chmod +x "$valgrind_script_"
		command_prefix="${valgrind_script_} ${command_prefix}"

		echo " --- valgrind enabled in this test case ($(valgrind --version)) --- "
		timeout_set_multiplier 15 # some tests need so big one
	fi
}

# Terminate valgrind processes to get complete valgrind logs from them
valgrind_terminate() {
	# Disable starting new valgrind processes
	local tmpfile="$TEMP_DIR/$(unique_file)_fake_valgrind.txt"
	echo -e "#!/usr/bin/env bash\n\"\$@\"" > "$tmpfile"
	chmod +x "$tmpfile"
	mv "$tmpfile" "$valgrind_script_"
	# Wait a bit if there are any valgrind processes which have just started. This is
	# because of a bug in glibc/valgrind which results in SIGSEGV if we kill it too soon.
	wait_for "! pgrep -u saunafstest -d, ${valgrind_tool_} | xargs -r ps -o etime= -p | grep -q '^ *00:0[0-3]$'" '5 seconds' || true
	local pattern="${valgrind_tool_}"
	if pgrep -u saunafstest "$pattern" >/dev/null; then
		echo " --- valgrind: Waiting for all processes to be terminated --- "
		pkill -TERM -u saunafstest "$pattern"
		wait_for "! pgrep -u saunafstest ${valgrind_tool_} >/dev/null" '60 seconds' || true
		if pgrep -u saunafstest "$pattern" >/dev/null; then
			echo " --- valgrind: Stop FAILED --- "
		else
			echo " --- valgrind: Stop OK --- "
		fi
	fi
	rm -f /tmp/vgdb-pipe*by-saunafstest* || true  # clean up any garbage left in /tmp
}

test_function_exists() {
	declare -f -F $1 > /dev/null
	return $?
}

# Call this in a test case to mark test as failed, but continue running
test_add_failure() {
	if test_frozen; then
		return
	fi
	local message="[$(date +"%F %T")] $*"
	# Env. variable ERROR_FILE can be used to store error messages in a file
	echo "$message" | tee -a "${ERROR_FILE:-/dev/null}" >> "$test_result_file"
	# Print bold red message to the console
	tput setaf 1
	tput bold
	echo "FAILURE $message"
	tput sgr0
}

# Call this in a test case to mark test as failed and stop it
test_fail() {
	if test_frozen; then
		return
	fi
	test_add_failure "(FATAL) $*"
	test_end
}

# Call this in a test case to make the result of a test not
# change when calling test_fail or test_add_failure
# Useful before cleaning the test using commands like killall
test_freeze_result() {
	touch "$test_end_file"
}

# This checks if the test if frozen
test_frozen() {
	[[ -f "$test_end_file" ]]
}

# You can call this function in a test case to immediately end the test.
# You don't have to; it will be called automatically at the end of the test.
test_end() {
	trap - DEBUG
	if [[ ${DEBUG} ]]; then
		set +x
	fi
	test_freeze_result
	# some tests may leave pwd at sfs mount point, causing a lockup when we stop sfs
	cd
	if valgrind_enabled; then
		valgrind_terminate
	fi
	# terminate all SaunaFS daemons if requested (eg. to collect some code coverage data)
	if [[ ${GENTLY_KILL:-} ]]; then
		for i in {1..50}; do
			local pattern='sfs'
			pkill -USR1 -u saunafstest "$pattern" || true
			if ! pgrep -u saunafstest "$pattern" >/dev/null; then
				echo "All SaunaFS processes terminated"
				break
			fi
			sleep 0.2
		done
	fi

	if parse_true "${ZONED_DISKS:-}" ; then
		remove_all_emulated_zoned_disks
	fi

	# Clean up FDB cluster if one was started for this test. LeilFS processes
	# must be stopped first: killing FDB under a live MDS wedges teardown.
	if [[ ${fdb_cluster_started:-} ]]; then
		terminate_fs_processes
		cleanup_fdb_cluster
	fi

	local errors=$(cat "$test_result_file")
	if [[ $errors ]]; then
		exit 1
	else
		# Remove syslog.log from ERROR_DIR, because it would cause the test to fail
		rm -f "$ERROR_DIR/syslog.log"
		exit 0
	fi
}

debug_command() {
	local depth=${#BASH_SOURCE[@]}
	if (( --depth < 1 )); then
		return 0;
	fi
	if (( depth <= DEBUG_LEVEL )); then
		local indent=">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>"
		local level=${indent:0:$depth}
		local source=$(basename "${BASH_SOURCE[1]}")
		local line=${BASH_LINENO[0]}
		local function=${FUNCNAME[1]}
		local command=${BASH_COMMAND}
		echo "${level} ${source}:${line}: ${function}(): ${command}" >&2 | true
	fi
}

function parametrize_command() {
	local cmd="${1}"
	shift
	local params=()
	local envs=()
	for param in "${@}" ; do
		if echo ${param} | grep -q '=' ; then
			envs+=(${param})
		else
			params+=(${param})
		fi
	done
	# echo "$(pwd):env=${envs[@]}, ${cmd} params=${params[@]}"
	env ${envs[@]} "${cmd}" ${params[@]}
}

# Do not run directly in test cases
# This should be called at the very beginning of a test
test_begin() {
	if ! is_windows_system; then
		( tail -n0 -f /var/log/syslog | stdbuf -oL tee "$ERROR_DIR/syslog.log" & )
	fi
	test_result_file="$TEMP_DIR/$(unique_file)_results.txt"
	test_end_file=$test_result_file.end
	check_configuration
	test_cleanup
	remove_all_emulated_zoned_disks
	touch "$test_result_file"
	trap 'trap - ERR; set +eE; catch_error_ "${BASH_SOURCE:-}" "${LINENO:-}" "${FUNCNAME:-}"; test_end' ERR
	set -E
	timeout_init
	system_init
	if [[ ${USE_VALGRIND} ]]; then
		valgrind_tool=${VALGRIND_TOOL:-}
		valgrind_enable ${valgrind_tool}
	fi
	if [[ ${DEBUG} ]]; then
		export PS4='+$(basename "${BASH_SOURCE:-}"):${LINENO:-}:${FUNCNAME[0]:+${FUNCNAME[0]}():} '
		set -x
	fi
	if (( $DEBUG_LEVEL >= 1 )); then
		trap 'debug_command' DEBUG
		set -T
	fi
}

# Try to chmod 777 all files and subdirectories in given directory using passwordless sudo
mass_chmod_777() {
	find "$1" -mindepth 1 -exec bash -c \
		'user=$(stat -c "%U" "{}"); sudo -nu $user setfacl -b "{}" ; sudo -nu $user chmod 777 "{}"' \;
}

# Unmounts all SaunaFS mountpoints created in Windows
windows_unmount_fs() {
	local retries=0
	# Search for all drvfs filesystems mounted and umount them
	while list_of_mounts=$(cat /proc/mounts | awk '$1 ~ /^[F-O]:/' | grep drvfs); do
		echo "$list_of_mounts" | awk '{print $2}' | xargs -r -d'\n' -n1 sudo umount -l || sleep 1
		if ((++retries == 30)); then
			echo "Can't unmount: $list_of_mounts" >&2
			break
		fi
	done
	taskkill.exe /IM ${SAFS_MOUNT_COMMAND} /F &> /dev/null || true
}

# Unmounts all SaunaFS testing mountpoints created in Unix-like OS's
unix_unmount_fs() {
	local retries=0
	pkill -KILL -u saunafstest sfsmount || true
	pkill -KILL -u saunafstest memcheck || true
	# Search for all fuse filesystems mounted by user saunafstest and umount them
	local uid=$(id -u saunafstest)
	while list_of_mounts=$(cat /proc/mounts | awk '$3 ~ /^fuse/' | grep "user_id=$uid[^0-9]"); do
		echo "$list_of_mounts" | awk '{print $2}' | xargs -r -d'\n' -n1 fusermount -u || sleep 1
		if ((++retries == 30)); then
			echo "Can't unmount: $list_of_mounts" >&2
			break
		fi
	done
}

# Stop mounts first (no FUSE op may hang on a dying MDS), then daemons:
# SIGTERM, bounded wait, SIGKILL. Run before cleanup_fdb_cluster -- FDB
# must never die under a live MDS or teardown wedges. On Windows only the
# client mount runs locally; unmount it and leave the rest to the caller.
terminate_fs_processes() {
	if is_windows_system; then
		windows_unmount_fs
		return
	fi
	local pattern='sfs'
	unix_unmount_fs
	pkill -TERM -u saunafstest "$pattern" || true
	for i in {1..25}; do
		if ! pgrep -u saunafstest "$pattern" >/dev/null; then
			return
		fi
		sleep 0.2
	done
	pkill -KILL -u saunafstest "$pattern" || true
}

# Do not use directly
# This removes all temporary files and unmounts filesystems
test_cleanup() {
	# Unmount all sfsmounts
	if is_windows_system; then
		windows_unmount_fs
	else
		unix_unmount_fs
	fi
	# Remove temporary files
	if ! [[ $TEMP_DIR ]]; then
		echo "TEMP_DIR variable empty, cowardly refusing to rm -rf /*"
		exit 1
	fi
	if ! rm -rf "$TEMP_DIR"/* 2>/dev/null; then
		# try to remove other users' junk the slow way
		mass_chmod_777 "$TEMP_DIR"
		rm -rf "$TEMP_DIR"/*
	fi
	# Clean ramdisk
	if [[ $RAMDISK_DIR ]]; then
		if ! rm -rf "$RAMDISK_DIR"/* 2>/dev/null; then
			mass_chmod_777 "$RAMDISK_DIR"
			rm -rf "$RAMDISK_DIR"/*
		fi
	fi

	# Clean the disks used by chunkservers
	for disk in ${SAUNAFS_DISKS} ${SAUNAFS_LOOP_DISKS}; do
		rm -rf "${disk}"/meta/chunks[0-9A-F][0-9A-F]
		rm -rf "${disk}"/data/chunks[0-9A-F][0-9A-F]
		rm -f "${disk}"/.lock
	done
}

catch_error_() {
	# Call test cleanup function
	if test_function_exists test_error_cleanup ; then
		test_error_cleanup || true
	fi

	local file=$1
	local line=$2
	local funcname=$3
	if [[ $funcname ]]; then
		local location="in function $funcname ($(basename "$file"):$line)"
	else
		local location="($file:$line)"
	fi
	# print_stack 1 removes catch_error_ from stack trace
	local stack=$(print_stack 1)
	local command=$(get_source_line "$file" "$line")
	test_add_failure "Command '$command' failed $location"$'\nBacktrace:\n'"$stack"
}

#!/usr/bin/env bash

# This file can be used to run tests as a user saunafstests
# To create such user:
#   sudo adduser --quiet --system --group --home /var/lib/saunafstest saunafstest
#   sudo usermod -a -G fuse saunafstest
# To work as a shell script it also needs the file /etc/sudoers.d/saunafstest;
# you can create this file with: sudo visudo -f /etc/sudoers.d/saunafstest
# The file should contain these two lines:
#   ALL ALL = (saunafstest) NOPASSWD: ALL
#   ALL ALL = NOPASSWD: /usr/bin/pkill -9 -u saunafstest

start_test() {
	local test_name=$1
	local test_env=$2

	if [[ -n $test_env ]]; then
		test_env=$test_env;
	fi

	test_script="set -eu; source tools/test_main.sh; test_begin; trap test_end INT; $test_env source '$test_name'; test_end"
	nice nice sudo -HEu saunafstest bash -c "$test_script"
	status=$?
	stop_tests
}

# If you want to run a test multiple times with different values.
# Include a generator statement inside of the test's source file.
# Example:
# @generator
# _wrapper() {
#     @callback@
# }
# @endgenerator
# If you want to pass additional parameters pass:
# _wrapper() {
#     @callback@ "key='your own long value'"
# }
unwrap_generators() {
	local start=$(cat $1 | grep -n "# @generator" | cut -d: -f1)
	local end=$(cat $1 | grep -n "# @endgenerator" | cut -d: -f1)
	if [[ $start ]] && [[ $end ]]; then
		start=$((start + 1))
		end=$((end - 1))
		wrapper_src=$(cat $1 | sed -ne "s+@callback@+start_test $1+g;${start},${end}s/#//p")
		echo "Using generator statement:"
		echo "$1:[$start, $end]:"
		echo "$wrapper_src"
		eval "$wrapper_src"
		_wrapper
	else
		start_test $1
	fi
}

stop_ganesha() {
	# umount all hanging SaunaFS mounts
	for mount in $(mount | grep /tmp/SaunaFS | awk '{print $3}'); do
		sudo umount -l $mount || echo "Failed to umount $mount"
	done

	# Check if ganesha daemon is running and kill it
	if pgrep ganesha.nfsd > /dev/null; then
		sudo pkill -9 ganesha.nfsd
		sleep 0.5
	fi

	# Check if ganesha daemon is still running
	if ps aux | grep [g]anesha > /dev/null; then
		# Start a new ganesha daemon to remove the hanging one
		sudo /usr/bin/ganesha.nfsd -L /var/log/ganesha.log
		sleep 2

		# Kill the newly started ganesha daemon to avoid conflicts with tests
		sudo pkill -9 ganesha.nfsd
	fi
}

stop_tests() {
	local users=$(echo saunafstest saunafstest_{0..9})
	local users_list=${users// /,}
	local try_count=0

	# check if there is a hanging ganesha daemon
	if pgrep ganesha.nfsd > /dev/null; then
		stop_ganesha
	fi

	# start with killing saunafstest processes, this will likely suffice
	sudo pkill -9 -u saunafstest
	sleep 0.1
	while pgrep -u $users_list >/dev/null ; do
		for user in $users; do
			sudo pkill -9 -u $user
		done
		((try_count++))
		if (( try_count == 50 )); then
			echo "Cannot stop running tests, still running:" >&2
			pgrep -u $users_list >&2
			exit 1
		fi
		sleep 0.5
	done
}

if [[ $# != 1 ]]; then
	echo "Usage: $0 <test_case>" >&2
	exit 1
fi
export SOURCE_DIR=$(readlink -m "$(dirname "$0")/..")
export ERROR_DIR=/tmp/saunafs_error_dir # Error dir content lifetime scope is a test suit
export SAUNAFS_LOG_ORIGIN=yes # adds file:line:function to debug logs
export SAUANFS_LOG_LEVEL="debug"

umask 0022
sudo rm -rf "${ERROR_DIR}"
mkdir -p "${ERROR_DIR}"
chmod 0777 "${ERROR_DIR}"

# Run the tests
cd "$(dirname "$0")"
stop_tests

unwrap_generators $1

nice nice sudo -HEu saunafstest sh -c "chmod -Rf a+rwX ${ERROR_DIR}"
for log_file in "$ERROR_DIR"/* ; do
	log_file_basename=$(basename "$log_file")
	if [[ -s ${log_file} ]]; then
		status=1
		if [[ $log_file_basename != syslog.log ]]; then
			# Do not inform users that there is nonempty syslog
			# It is always nonempty if the test failed
			echo "(FATAL) Errors in ${log_file_basename}" | tee "${ERROR_FILE}" #error file lifetime is: all tests
			# print all non-binary files to stdout
			# awk will return 0 if it doesn't find binary, and 1 if
			# it does, which is what we want for the return value
			if file --mime-encoding "${log_file}" | awk '{exit $2=="binary"}'; then
				cat "${log_file}"
			else
				# TEMPORARY DIAGNOSTIC (do not merge): a binary file here is never printed, so
				# when one shows up we cannot tell what it is. Identify it instead, in
				# particular whether it is a core dump, which would mean a process crashed
				# rather than merely being reported on.
				echo "(binary error file, contents not printed)"
				ls -l "${log_file}" | awk '{print "    size: " $5 " bytes"}'
				echo "    file: $(file -b "${log_file}" 2>&1)"
				readelf -h "${log_file}" 2>/dev/null | sed -n '1,12p' | sed 's/^/    readelf: /' || true
			fi
		fi
		if [[ $TEST_OUTPUT_DIR ]]; then
			cp "${log_file}" "$TEST_OUTPUT_DIR/$(date '+%F_%T')__$(basename $1 .sh)__${log_file_basename}"
		fi
	fi
done

rm -rf "${ERROR_DIR}"

# Return proper status
exit $status

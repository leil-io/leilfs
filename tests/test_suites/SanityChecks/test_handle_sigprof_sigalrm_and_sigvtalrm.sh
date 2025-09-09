timeout_set 45 seconds

CHUNKSERVERS=1 \
	MASTERSERVERS=2 \
	USE_RAMDISK="YES" \
	setup_local_empty_saunafs info

# Function to validate signal handling
validate_signal_handling() {
	local component="$1"
	local pid="$2"
	local signal="$3"

	echo "Sending ${signal} signal to ${component} (PID: ${pid})..."
	kill -s "${signal}" "${pid}"

	# Wait a moment for signal handling
	sleep 2

	# Verify process is still alive
	if ! kill -0 "${pid}" 2>/dev/null; then
		echo "ERROR: ${component} process ${pid} died after receiving ${signal}"
		return 1
	fi

	echo "${component} survived ${signal} signal correctly"
	return 0
}

# Add a shadow master and wait until it's fully synchronized
saunafs_master_n 1 start
assert_eventually "saunafs_shadow_synchronized 1"

# Generate some changes to ensure shadow is actively working
touch "${info[mount0]}"/test_file_before_signals
assert_eventually "saunafs_shadow_synchronized 1"

# Add a metalogger
saunafs_metalogger_daemon start

# Test SIGPROF, SIGVTALRM and SIGALRM on shadow master
shadow_pid=$(saunafs_master_n 1 test | sed 's/.*: //')
assert_matches "^[0-9]+$" "$shadow_pid"

echo "Testing shadow master signal handling (PID: ${shadow_pid})"
validate_signal_handling "shadow_master" "${shadow_pid}" "SIGPROF"
validate_signal_handling "shadow_master" "${shadow_pid}" "SIGVTALRM"
validate_signal_handling "shadow_master" "${shadow_pid}" "SIGALRM"

# Test signals on primary master
master_pid=$(saunafs_master_daemon test | sed 's/.*: //')
assert_matches "^[0-9]+$" "$master_pid"

echo "Testing primary master signal handling (PID: ${master_pid})"
validate_signal_handling "primary_master" "${master_pid}" "SIGPROF"
validate_signal_handling "primary_master" "${master_pid}" "SIGVTALRM"
validate_signal_handling "primary_master" "${master_pid}" "SIGALRM"

# Test signals on chunkserver
chunkserver_pid=$(saunafs_chunkserver_daemon 0 test | sed 's/.*pid: //')
assert_matches "^[0-9]+$" "$chunkserver_pid"

echo "Testing chunkserver signal handling (PID: ${chunkserver_pid})"
validate_signal_handling "chunkserver" "${chunkserver_pid}" "SIGPROF"
validate_signal_handling "chunkserver" "${chunkserver_pid}" "SIGVTALRM"
validate_signal_handling "chunkserver" "${chunkserver_pid}" "SIGALRM"

# Test signals on metalogger
metalogger_pid=$(saunafs_metalogger_daemon test | sed 's/.*: //')
assert_matches "^[0-9]+$" "$metalogger_pid"

echo "Testing metalogger signal handling (PID: ${metalogger_pid})"
validate_signal_handling "metalogger" "${metalogger_pid}" "SIGPROF"
validate_signal_handling "metalogger" "${metalogger_pid}" "SIGVTALRM"
validate_signal_handling "metalogger" "${metalogger_pid}" "SIGALRM"

# Verify all components are still functional after signal testing
echo "Verifying system functionality after signal testing..."

# Generate more changes to test master functionality
touch "${info[mount0]}"/test_file_after_signals
assert_eventually "saunafs_shadow_synchronized 1"

# Test file operations through chunkserver
echo "test_data" > "${info[mount0]}"/test_file_write
assert_success test -f "${info[mount0]}"/test_file_write
assert_equals "test_data" "$(cat "${info[mount0]}"/test_file_write)"

echo "All signal handling tests completed successfully"

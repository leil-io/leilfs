# Shared setup and assertions for disk-state reports delayed across a version
# operation. The caller selects busy or settled through disk_state_report_order.

master_extra_config='CHUNK_REGISTRATION_CHUNKS_PER_SECOND = 5'
master_extra_config+='|CHUNK_REGISTRATION_BULK_SIZE = 1'
master_extra_config+='|CHUNKS_LOOP_MIN_TIME = 1'
master_extra_config+='|CHUNKS_LOOP_MAX_CPU = 90'
master_extra_config+='|OPERATIONS_DELAY_INIT = 0'
master_extra_config+='|OPERATIONS_DELAY_DISCONNECT = 0'

CHUNKSERVERS=2 \
	DISK_PER_CHUNKSERVER=1 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
	MASTER_EXTRA_CONFIG="$master_extra_config" \
	CHUNKSERVER_EXTRA_CONFIG='MASTER_RECONNECTION_DELAY = 1' \
	MOUNT_EXTRA_CONFIG='sfscachemode=NEVER,sfsioretries=3' \
	setup_local_empty_saunafs info

# Place the only copy on chunkserver 0; chunkserver 1 will receive evacuation.
# The held report hides the disk flag until the intended version operation.
saunafs_chunkserver_daemon 1 stop

target_dir="${info[mount0]}/target_dir"
mkdir "$target_dir"
assert_success saunafs setgoal 1 "$target_dir"

target_file="$target_dir/target"
FILE_SIZE=1M file-generate "$target_file"
assert_eventually_equals 'echo 1' "count_registered_parts '$target_file'"

# Prepare the expected overwrite contents without changing the target yet.
expected_file="$TEMP_DIR/expected"
cp "$target_file" "$expected_file"
dd if=/dev/zero of="$expected_file" bs=4096 count=1 conv=notrunc status=none

chunk_name=$(saunafs fileinfo "$target_file" | awk '/chunk 0:/ {print $3; exit}')
assert_success test -n "$chunk_name"

target_chunk=${chunk_name%%_*}
initial_version=$((16#${chunk_name##*_}))

source_disk="$RAMDISK_DIR/hdd_0_0"
destination_disk="$RAMDISK_DIR/hdd_1_0"
assert_equals 1 "$(find "$source_disk" -type f -name 'chunk*.dat' | wc -l)"

# Complete registration through the proxy before changing the disk state.
# No ongoing sweep or later reconnect may supply a corrective report.
get_next_port_number proxy_port

ready_marker="$TEMP_DIR/proxy-ready"
registered_marker="$TEMP_DIR/registration-ended"
held_marker="$TEMP_DIR/disk-report-held"
status_marker="$TEMP_DIR/version-status-forwarded"
release_marker="$TEMP_DIR/release-disk-report"
delivered_marker="$TEMP_DIR/disk-report-delivered"
proxy_log="$TEMP_DIR/disk-state-version-proxy.log"

python3 "${SOURCE_DIR}/tests/tools/disk_state_version_proxy.py" \
	--host "$(get_ip_addr)" --listen-port "$proxy_port" --master-port "${info[matocs]}" \
	--target-chunk "0x$target_chunk" --order "$disk_state_report_order" \
	--ready-marker "$ready_marker" --registered-marker "$registered_marker" \
	--held-marker "$held_marker" --status-marker "$status_marker" \
	--release-marker "$release_marker" --delivered-marker "$delivered_marker" \
	> "$proxy_log" 2>&1 &
proxy_pid=$!

trap '
	kill "$proxy_pid" 2>/dev/null || true
	wait "$proxy_pid" 2>/dev/null || true
' EXIT

assert_eventually 'test -f "$ready_marker"'

# Route CS0 through the proxy so the disk report and version command can be
# ordered explicitly. All traffic still reaches the same master.
add_lines_sfschunkserver_cfg_ "MASTER_HOST = $(get_ip_addr)|MASTER_PORT = $proxy_port" 0
saunafs_chunkserver_daemon 0 restart
assert_eventually 'test -f "$registered_marker"'
assert_eventually_equals 'echo 1' "count_registered_parts '$target_file'"

# Make an evacuation destination available before marking the source disk.
saunafs_chunkserver_daemon 1 start
saunafs_wait_for_all_ready_chunkservers
wait_for_chunkserver_pull_registration 1

# Queue a real (v, to-delete) announcement, held before the master sees it.
sed -i '1s/^/*/' "${info[chunkserver0_hdd]}"
saunafs_chunkserver_daemon 0 reload
assert_eventually 'test -f "$held_marker"'

# Busy mode releases the report before SET_VERSION reaches the worker.
# Settled mode keeps it held until the write and its version reply have finished.
MESSAGE='the overwrite must succeed without downgrading or invalidating the copy' \
	assert_success dd if=/dev/zero of="$target_file" bs=4096 count=1 \
		conv=notrunc,fsync status=none

assert_eventually 'test -f "$status_marker"'

if [[ "$disk_state_report_order" == settled ]]; then
	assert_failure test -f "$delivered_marker"
	touch "$release_marker"
fi

assert_eventually 'test -f "$delivered_marker"'
assert_success kill -0 "$proxy_pid"

cat "$proxy_log"

# The overwrite must retain the chunk ID, increment its version once and preserve data.
current_chunk=$(saunafs fileinfo "$target_file" | awk '/chunk 0:/ {print $3; exit}')
assert_equals "$target_chunk" "${current_chunk%%_*}"
assert_equals "$((initial_version + 1))" "$((16#${current_chunk##*_}))"

assert_eventually_equals 'echo 1' "count_registered_parts '$target_file'"
assert_files_equal "$expected_file" "$target_file"

# Maintenance is already enabled; BUSY/locked copies cannot be evacuated until
# their write settles. No further reload or reconnect may repair the lost flag.
MESSAGE="stale disk-state report ($disk_state_report_order) must still trigger evacuation" \
	assert_eventually_prints 0 "find '$source_disk' -type f -name 'chunk*' | wc -l" '15 seconds'

assert_eventually_prints 1 "find '$destination_disk' -type f -name 'chunk*.dat' | wc -l"
assert_eventually_equals 'echo 1' "count_registered_parts '$target_file'"

assert_success kill -0 "$proxy_pid"
assert_files_equal "$expected_file" "$target_file"

timeout_set '1 minute'

# After an initial zero-space report, refresh capacity with pull credits held.
# Allow new replicas, but defer repair of existing, unreported replicas.

CHUNKSERVERS=2 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
	MASTER_EXTRA_CONFIG='CHUNK_REGISTRATION_CHUNKS_PER_SECOND = 5|CHUNK_REGISTRATION_BULK_SIZE = 1|CHUNKS_LOOP_MIN_TIME = 1|CHUNKS_LOOP_MAX_CPU = 90|OPERATIONS_DELAY_INIT = 0|OPERATIONS_DELAY_DISCONNECT = 0' \
	CHUNKSERVER_EXTRA_CONFIG='MASTER_RECONNECTION_DELAY = 1' \
	MOUNT_EXTRA_CONFIG='sfscachemode=NEVER,sfsioretries=3' \
	setup_local_empty_saunafs info

data_dir="${info[mount0]}/goal2"
mkdir "$data_dir"
assert_success saunafs setgoal 2 "$data_dir"

seed="$data_dir/seed"
FILE_SIZE=4K file-generate "$seed"
assert_eventually_equals 'echo 2' "count_registered_parts '$seed'"

get_next_port_number proxy_port
zero_marker="$TEMP_DIR/initial-zero-space"
space_marker="$TEMP_DIR/positive-space-update"
release_marker="$TEMP_DIR/release-registration-credits"
end_marker="$TEMP_DIR/registration-ended"
maintenance_marker="$TEMP_DIR/maintenance-loops-completed"
repair_marker="$TEMP_DIR/premature-repair-request"
proxy_log="$TEMP_DIR/registration-space-proxy.log"

python3 "${SOURCE_DIR}/tests/tools/registration_space_proxy.py" \
	--host "$(get_ip_addr)" --listen-port "$proxy_port" --master-port "${info[matocs]}" \
	--zero-marker "$zero_marker" --space-marker "$space_marker" \
	--release-marker "$release_marker" --end-marker "$end_marker" \
	--client-port "${info[matocl]}" --maintenance-marker "$maintenance_marker" \
	--repair-marker "$repair_marker" >"$proxy_log" 2>&1 &
proxy_pid=$!
trap 'kill "$proxy_pid" 2>/dev/null || true; wait "$proxy_pid" 2>/dev/null || true' EXIT

add_lines_sfschunkserver_cfg_ "MASTER_HOST = $(get_ip_addr)|MASTER_PORT = $proxy_port" 1
saunafs_chunkserver_daemon 1 restart
assert_eventually 'test -f "$zero_marker"'

MESSAGE='disk space must refresh while pull credits are held' \
	assert_eventually 'test -f "$space_marker"' '10 seconds'
assert_equals 1 "$(count_registered_parts "$seed")"

new_file="$data_dir/created_during_registration"
FILE_SIZE=4K assert_success file-generate "$new_file"
assert_success file-validate "$new_file"
assert_eventually_equals 'echo 2' "count_registered_parts '$new_file'"

MESSAGE='new-chunk allocation must precede registration completion' \
	assert_failure test -f "$end_marker"

# Let maintenance run to expose premature repair of the unreported replica.
assert_eventually 'test -f "$maintenance_marker"' '15 seconds'

MESSAGE='background repair must not target an unfinished pull inventory' \
	assert_failure test -f "$repair_marker"
assert_equals 1 "$(count_registered_parts "$seed")"

# Release credits and verify inventory convergence and existing data.
touch "$release_marker"
assert_eventually 'test -f "$end_marker"'

assert_eventually_equals 'echo 2' "count_registered_parts '$seed'"
assert_success file-validate "$seed"

# Space reports must not be logged as registration completion.
syslog_file="${ERROR_DIR}/syslog.log"
assert_eventually 'grep -Eq "registration space report received .*usedspace: 0 .*totalspace: 0 " "$syslog_file"'
assert_failure grep -q 'chunkserver register end (packet version: 5)' "$syslog_file"

timeout_set '1 minute'

# Suppress START after early REGISTER_SPACE to trigger push fallback.
# Hold the inventory tail: push bulks must not renew pull credits or enable
# repair of existing, unreported replicas.

master_extra_config='CHUNK_REGISTRATION_CHUNKS_PER_SECOND = 10'
master_extra_config+='|CHUNK_REGISTRATION_BULK_SIZE = 1'
master_extra_config+='|CHUNK_REGISTRATION_WINDOW = 1'
master_extra_config+='|CHUNKS_LOOP_MIN_TIME = 1'
master_extra_config+='|CHUNKS_LOOP_MAX_CPU = 90'
master_extra_config+='|OPERATIONS_DELAY_INIT = 0'
master_extra_config+='|OPERATIONS_DELAY_DISCONNECT = 0'

chunkserver_extra_config='MASTER_RECONNECTION_DELAY = 1'
chunkserver_extra_config+='|CHUNK_REGISTRATION_START_TIMEOUT = 3'

# Separate bulks let the proxy hold "target" and forward "other".
chunkserver_extra_config+='|HDD_CHUNK_BULK_SIZE = 1'

CHUNKSERVERS=2 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
	MASTER_EXTRA_CONFIG="$master_extra_config" \
	CHUNKSERVER_EXTRA_CONFIG="$chunkserver_extra_config" \
	MOUNT_EXTRA_CONFIG='sfscachemode=NEVER,sfsioretries=3' \
	setup_local_empty_saunafs info

# Forward "other" to detect renewed credits; keep "target" unreported.
data_dir="${info[mount0]}/goal2"
mkdir "$data_dir"
assert_success saunafs setgoal 2 "$data_dir"

target_file="$data_dir/target"
FILE_SIZE=4K file-generate "$target_file" "$data_dir/other"
assert_eventually_equals 'echo 2' "count_registered_parts '$target_file'"
assert_eventually_equals 'echo 2' "count_registered_parts '$data_dir/other'"

target_chunk=$(saunafs fileinfo "$target_file" | awk '/chunk 0:/ { print $3; exit }')
assert_success test -n "$target_chunk"
target_chunk=${target_chunk%%_*}

get_next_port_number proxy_port
ready_marker="$TEMP_DIR/proxy-ready"
space_marker="$TEMP_DIR/early-space-forwarded"
opening_marker="$TEMP_DIR/opening-credit-received"
bulk_marker="$TEMP_DIR/fallback-bulk-forwarded"
maintenance_marker="$TEMP_DIR/maintenance-loops-completed"
credit_marker="$TEMP_DIR/unexpected-fallback-credit"
repair_marker="$TEMP_DIR/premature-repair-request"
release_marker="$TEMP_DIR/release-fallback-tail"
end_marker="$TEMP_DIR/registration-ended"
proxy_log="$TEMP_DIR/registration-timeout-proxy.log"

# Proxy markers synchronize checks with protocol events and maintenance passes.
python3 "${SOURCE_DIR}/tests/tools/registration_timeout_proxy.py" \
	--host "$(get_ip_addr)" --listen-port "$proxy_port" --master-port "${info[matocs]}" \
	--client-port "${info[matocl]}" --target-chunk "0x$target_chunk" \
	--ready-marker "$ready_marker" --space-marker "$space_marker" \
	--opening-marker "$opening_marker" --bulk-marker "$bulk_marker" \
	--maintenance-marker "$maintenance_marker" --credit-marker "$credit_marker" \
	--repair-marker "$repair_marker" --release-marker "$release_marker" \
	--end-marker "$end_marker" >"$proxy_log" 2>&1 &
proxy_pid=$!
trap 'kill "$proxy_pid" 2>/dev/null || true; wait "$proxy_pid" 2>/dev/null || true' EXIT

assert_eventually 'test -f "$ready_marker"'

# Keep server 0 serving data while server 1 reconnects through the proxy.
add_lines_sfschunkserver_cfg_ "MASTER_HOST = $(get_ip_addr)|MASTER_PORT = $proxy_port" 1
saunafs_chunkserver_daemon 1 restart
assert_eventually 'test -f "$space_marker"'

# The opening credit is valid and confirms acceptance of early space.
assert_eventually 'test -f "$opening_marker"'

# Once push fallback starts, the held target must still have one known replica.
assert_eventually 'test -f "$bulk_marker"'
assert_equals 1 "$(count_registered_parts "$target_file")"

# Let maintenance run with incomplete inventory to expose premature repair.
assert_eventually 'test -f "$maintenance_marker"' '15 seconds'

MESSAGE='timeout fallback bulks must not generate pull credits' \
	assert_failure test -f "$credit_marker"

MESSAGE='fallback must not enable repair before its inventory finishes' \
	assert_failure test -f "$repair_marker"

assert_failure test -f "$end_marker"

# Release the target and tail, then verify registration and data converge.
touch "$release_marker"
assert_eventually 'test -f "$end_marker"'

assert_eventually_equals 'echo 2' "count_registered_parts '$target_file'"
assert_success file-validate "$target_file" "$data_dir/other"

assert_eventually 'grep -q "falling back to push registration" "$ERROR_DIR/syslog.log"'

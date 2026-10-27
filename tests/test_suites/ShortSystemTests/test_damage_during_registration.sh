timeout_set '1 minute'

# An early EC damage report must remain authoritative when its inventory
# record arrives later. Inject damage before the first bulk on one connection.

master_extra_config='CHUNK_REGISTRATION_CHUNKS_PER_SECOND = 5'
master_extra_config+='|CHUNK_REGISTRATION_BULK_SIZE = 1'
master_extra_config+='|CHUNK_REGISTRATION_WINDOW = 1'

# Delay background repair so only registration can change the part count.
master_extra_config+='|OPERATIONS_DELAY_INIT = 3600'

CHUNKSERVERS=3 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
	MASTER_CUSTOM_GOALS='10 ec21: $ec(2,1)' \
	MASTER_EXTRA_CONFIG="$master_extra_config" \
	CHUNKSERVER_EXTRA_CONFIG='MASTER_RECONNECTION_DELAY = 1' \
	MOUNT_EXTRA_CONFIG='sfscachemode=NEVER' \
	setup_local_empty_saunafs info

# Seed an EC(2,1) file with three healthy parts on separate chunkservers.
data_dir="${info[mount0]}/ec21"
mkdir "$data_dir"
assert_success saunafs setgoal ec21 "$data_dir"

file="$data_dir/seed"
FILE_SIZE=4K file-generate "$file"
assert_eventually_equals 'echo 3' "count_registered_parts '$file'"

# The proxy allows one inventory bulk, then holds credits until released.
get_next_port_number proxy_port
ready_marker="$TEMP_DIR/credit-gate-ready"
held_marker="$TEMP_DIR/registration-credits-held"
release_marker="$TEMP_DIR/release-registration"
end_marker="$TEMP_DIR/registration-ended"

# Inject damage for the first bulk's actual EC part type before forwarding it.
# This models damage discovered before the part has been advertised.
python3 "${SOURCE_DIR}/tests/tools/registration_credit_gate.py" \
	--host "$(get_ip_addr)" --listen-port "$proxy_port" --master-port "${info[matocs]}" \
	--ready-marker "$ready_marker" --held-marker "$held_marker" \
	--release-marker "$release_marker" --end-marker "$end_marker" \
	--damage-first-bulk > "$TEMP_DIR/registration-credit-gate.log" 2>&1 &
proxy_pid=$!
trap 'kill "$proxy_pid" 2>/dev/null || true; wait "$proxy_pid" 2>/dev/null || true' EXIT
assert_eventually 'test -f "$ready_marker"'

# Reconnect one chunkserver through the proxy to control report ordering.
add_lines_sfschunkserver_cfg_ "MASTER_HOST = $(get_ip_addr)|MASTER_PORT = $proxy_port" 1
saunafs_chunkserver_daemon 1 restart

# The later inventory record must not make the damaged part usable again.
assert_eventually 'test -f "$held_marker"'
MESSAGE='registration must not restore an EC part reported damaged before its first bulk' \
	assert_equals 2 "$(count_registered_parts "$file")"
assert_failure test -f "$end_marker"

# Finish registration and verify the damage persists after the final packet.
touch "$release_marker"
assert_eventually 'test -f "$end_marker"'
assert_equals 2 "$(count_registered_parts "$file")"

# The two remaining healthy EC parts must still be sufficient to read the file.
assert_success file-validate "$file"

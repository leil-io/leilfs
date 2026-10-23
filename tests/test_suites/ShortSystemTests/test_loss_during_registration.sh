timeout_set '1 minute'

# Report one copy, then hold registration open while removing its disk.
# The master must forget the lost copy before inventory completion.

master_extra_config='CHUNK_REGISTRATION_CHUNKS_PER_SECOND = 5'
master_extra_config+='|CHUNK_REGISTRATION_BULK_SIZE = 1'
master_extra_config+='|CHUNK_REGISTRATION_WINDOW = 1'

CHUNKSERVERS=1 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
	MASTER_EXTRA_CONFIG="$master_extra_config" \
	CHUNKSERVER_EXTRA_CONFIG='MASTER_RECONNECTION_DELAY = 1' \
	setup_local_empty_saunafs info

# Seed metadata offline with four files, each containing one mock chunk.
saunafs_master_daemon stop
metadata_version=$(metadata_get_version "${info[master_data_path]}/metadata.sfs")
generate_mock_chunks_changelog 4 1 "$metadata_version" \
	> "${info[master_data_path]}/changelog.sfs"
assert_success sfsmetarestore -a -d "${info[master_data_path]}"
saunafs_master_daemon start
saunafs_wait_for_all_ready_chunkservers

# Add the matching mock disk and establish a fully registered baseline.
mock_dir="$RAMDISK_DIR/loss_during_registration"
mkdir -p "$mock_dir"
echo "mock:4:1:$mock_dir" >> "${info[chunkserver0_hdd]}"
saunafs_chunkserver_daemon 0 reload
assert_eventually_equals 'echo 4' 'count_chunk_copies'

# Allow one inventory bulk, then hold credits while loss reports stay live.
get_next_port_number proxy_port
ready_marker="$TEMP_DIR/credit-gate-ready"
held_marker="$TEMP_DIR/registration-credits-held"
release_marker="$TEMP_DIR/release-registration"
end_marker="$TEMP_DIR/registration-ended"
lost_marker="$TEMP_DIR/loss-forwarded"

# Markers distinguish a forwarded loss report from registration completion.
python3 "${SOURCE_DIR}/tests/tools/registration_credit_gate.py" \
	--host "$(get_ip_addr)" --listen-port "$proxy_port" --master-port "${info[matocs]}" \
	--ready-marker "$ready_marker" --held-marker "$held_marker" \
	--release-marker "$release_marker" --end-marker "$end_marker" \
	--lost-marker "$lost_marker" > "$TEMP_DIR/registration-credit-gate.log" 2>&1 &
proxy_pid=$!
trap 'kill "$proxy_pid" 2>/dev/null || true; wait "$proxy_pid" 2>/dev/null || true' EXIT
assert_eventually 'test -f "$ready_marker"'

# Reconnect through the proxy; only one of the four copies may be advertised.
add_lines_sfschunkserver_cfg_ "MASTER_HOST = $(get_ip_addr)|MASTER_PORT = $proxy_port" 0
saunafs_chunkserver_daemon 0 restart

assert_eventually 'test -f "$held_marker"'
assert_equals 1 "$(count_chunk_copies)"
assert_failure test -f "$end_marker"

# Keep the ordinary disk configured; remove only the mock disk holding all four chunks.
sed -i '/^mock:/d' "${info[chunkserver0_hdd]}"
saunafs_chunkserver_daemon 0 reload

# Loss must reach the master and remove the known copy before registration ends.
MESSAGE='disk loss must be reported while registration credits remain held' \
	assert_eventually 'test -f "$lost_marker"' '10 seconds'
assert_eventually_equals 'echo 0' 'count_chunk_copies' '10 seconds'
assert_failure test -f "$end_marker"

# Release inventory credits and ensure completion does not restore lost copies.
touch "$release_marker"
assert_eventually 'test -f "$end_marker"'
assert_equals 0 "$(count_chunk_copies)"

timeout_set '2 minutes'

# A priority on-demand query reply is deliberately allowed to pass queued pull
# registration packets. This test forces the resulting wire order for one
# existing chunk:
#
#   old REGISTER_CHUNKS(v) -- held by the proxy
#   QUERY_CHUNKS_RESPONSE(v)
#   SET_VERSION(v -> v+1)
#   old REGISTER_CHUNKS(v) -- released after SET_VERSION
#
# The master must keep the BUSY v+1 part that belongs to the in-flight write.
# Replacing it with the delayed v report makes the successful set-version
# status fail the write because it no longer finds a busy, valid part.

CHUNKSERVERS=1 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
	MASTER_EXTRA_CONFIG='CHUNK_REGISTRATION_CHUNKS_PER_SECOND = 5|CHUNK_REGISTRATION_BULK_SIZE = 1' \
	CHUNKSERVER_EXTRA_CONFIG='MASTER_RECONNECTION_DELAY = 1' \
	MOUNT_EXTRA_CONFIG='sfscachemode=NEVER,sfsioretries=3' \
	setup_local_empty_saunafs info

target_file="${info[mount0]}/target"
dd if=/dev/zero of="$target_file" bs=4096 count=1 conv=fsync status=none
assert_eventually_equals 'echo 1' "count_registered_parts '$target_file'"
target_chunk=$(saunafs fileinfo "$target_file" | awk '/chunk 0:/ { print $3; exit }')
MESSAGE='target file must have a chunk id' assert_success test -n "$target_chunk"

target_chunk=${target_chunk%%_*}
# Reconnect through a transparent proxy. It holds only the target's old
# registration packet; all other traffic remains unmodified.
get_next_port_number proxy_port
proxy_marker="$TEMP_DIR/registration-packet-held"
proxy_log="$TEMP_DIR/registration-reordering-proxy.log"
python3 "${SOURCE_DIR}/tests/tools/registration_reordering_proxy.py" \
	--listen-host "$(get_ip_addr)" \
	--listen-port "$proxy_port" \
	--master-host "$(get_ip_addr)" \
	--master-port "${info[matocs]}" \
	--target-chunk "0x$target_chunk" \
	--held-marker "$proxy_marker" >"$proxy_log" 2>&1 &
proxy_pid=$!
trap 'kill "$proxy_pid" 2>/dev/null || true; wait "$proxy_pid" 2>/dev/null || true' EXIT

add_lines_sfschunkserver_cfg_ "MASTER_HOST = $(get_ip_addr)|MASTER_PORT = $proxy_port" 0
saunafs_chunkserver_daemon 0 restart

assert_eventually 'test -f "$proxy_marker"'
MESSAGE='the target must still be unknown while its registration packet is held' \
	assert_equals 0 "$(count_registered_parts "$target_file")"

# This write causes the on-demand query. The proxy releases the stale packet
# only after the master has started the v+1 set-version operation.
MESSAGE='write must survive a delayed pre-operation registration report' \
	assert_success dd if=/dev/zero of="$target_file" bs=4096 count=1 conv=notrunc,fsync status=none

MESSAGE='proxy must exercise the reordered packet sequence' \
	assert_eventually 'grep -q "released stale registration after set-version" "$proxy_log"'
assert_eventually_equals 'echo 1' "count_registered_parts '$target_file'"

timeout_set '2 minutes'

# A version-changing write must not proceed when discovery times out, is disabled,
# or reaches its query limit. Hold the second replica's inventory; queries stay live.

# @generator
# _wrapper() {
#     @callback@ "DISCOVERY_FAILURE_MODE=timeout"
#     @callback@ "DISCOVERY_FAILURE_MODE=disabled"
#     @callback@ "DISCOVERY_FAILURE_MODE=limit"
# }
# @endgenerator

create_bdb_name_storage_entry_() {
	echo 'USE_BDB_FOR_NAME_STORAGE = 0'
}

query_limit=10000
if [[ "$DISCOVERY_FAILURE_MODE" == disabled ]]; then
	query_limit=0
elif [[ "$DISCOVERY_FAILURE_MODE" == limit ]]; then
	query_limit=1
fi

master_config='CHUNK_REGISTRATION_CHUNKS_PER_SECOND = 100'
master_config+='|CHUNK_REGISTRATION_BULK_SIZE = 1|CHUNK_REGISTRATION_WINDOW = 1'
master_config+='|ON_DEMAND_CHUNK_QUERY_TIMEOUT_MS = 3000'
master_config+="|ON_DEMAND_CHUNK_QUERY_LIMIT = $query_limit"

MASTERSERVERS=2 \
	CHUNKSERVERS=2 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
	MASTER_EXTRA_CONFIG="$master_config" \
	CHUNKSERVER_EXTRA_CONFIG='MASTER_RECONNECTION_DELAY = 1' \
	MOUNT_EXTRA_CONFIG='sfscachemode=NEVER,sfsioretries=1' \
	setup_local_empty_saunafs info

target_dir="${info[mount0]}/target_dir"
mkdir "$target_dir"
assert_success saunafs setgoal 2 "$target_dir"
target_file="$target_dir/target"
FILE_SIZE=1M file-generate "$target_file" "$target_dir/other"
assert_eventually_equals 'echo 2' "count_registered_parts '$target_file'"
cp "$target_file" "$TEMP_DIR/original"
assert_eventually_equals 'echo 2' "count_registered_parts '$target_dir/other'"
cp "$target_dir/other" "$TEMP_DIR/other-original"

chunk_version() {
	saunafs fileinfo "${1:-$target_file}" | awk '/chunk 0:/ {print $3; exit}'
}

original_version=$(chunk_version)
other_version=$(chunk_version "$target_dir/other")
saunafs_master_n 1 start
assert_eventually 'saunafs_shadow_synchronized 1'
saunafs_chunkserver_daemon 1 stop
saunafs_master_daemon kill
saunafs_make_conf_for_shadow 0
saunafs_make_conf_for_master 1
saunafs_master_daemon reload

get_next_port_number proxy_port
ready="$TEMP_DIR/gate-ready"
held="$TEMP_DIR/credits-held"
dropped="$TEMP_DIR/query-dropped"

python3 "${SOURCE_DIR}/tests/tools/registration_credit_gate.py" \
	--host "$(get_ip_addr)" --listen-port "$proxy_port" --master-port "${info[matocs]}" \
	--initial-credits 0 --ready-marker "$ready" --held-marker "$held" \
	--release-marker "$TEMP_DIR/release" --end-marker "$TEMP_DIR/end" \
	--drop-query-responses --dropped-query-marker "$dropped" \
	> "$TEMP_DIR/gate.log" 2>&1 &
proxy_pid=$!
trap 'kill "$proxy_pid" 2>/dev/null || true; wait "$proxy_pid" 2>/dev/null || true' EXIT
assert_eventually 'test -f "$ready"'

add_lines_sfschunkserver_cfg_ "MASTER_HOST = $(get_ip_addr)|MASTER_PORT = $proxy_port" 1
saunafs_chunkserver_daemon 1 start
assert_eventually 'test -f "$held"'
assert_eventually_equals 'echo 1' "count_registered_parts '$target_file'"
assert_eventually_equals 'echo 1' "count_registered_parts '$target_dir/other'"

dd if=/dev/zero of="$target_file" bs=4096 count=1 conv=notrunc,fsync \
	> "$TEMP_DIR/write.log" 2>&1 &
writer_pid=$!

if [[ "$DISCOVERY_FAILURE_MODE" != disabled ]]; then
	# The peer answered, but its reply cannot reach the master. No version
	# command may have been sent before this query completed.
	assert_eventually 'test -f "$dropped"'
	assert_equals "$original_version" "$(chunk_version)"
	assert_success kill -0 "$writer_pid"
fi

if [[ "$DISCOVERY_FAILURE_MODE" == limit ]]; then
	# The first write occupies the only query slot. A different chunk cannot
	# use that confirmation or proceed without admitting its own discovery.
	assert_failure dd if=/dev/zero of="$target_dir/other" bs=4096 count=1 \
		conv=notrunc,fsync status=none
	assert_eventually 'grep -q "on-demand chunk-location query limit reached (1)" "$ERROR_DIR/syslog.log"'
	assert_equals "$other_version" "$(chunk_version "$target_dir/other")"
	assert_success cmp "$TEMP_DIR/other-original" "$target_dir/other"
fi

write_status=0
wait "$writer_pid" || write_status=$?
MESSAGE='failed discovery must refuse the write instead of updating one replica' \
	assert_failure test "$write_status" -eq 0

assert_success kill -0 "$proxy_pid"
assert_failure test -f "$TEMP_DIR/end"
assert_equals "$original_version" "$(chunk_version)"
assert_success cmp "$TEMP_DIR/original" "$target_file"
# Query backpressure must not close the separate path for allocating new chunks.
echo 'new allocation' > "${info[mount0]}/new"
assert_equals 'new allocation' "$(cat "${info[mount0]}/new")"
echo "DISCOVERY_FAILURE_VERIFIED: $DISCOVERY_FAILURE_MODE, write status $write_status"

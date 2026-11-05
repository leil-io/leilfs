timeout_set '2 minutes'

# Discovery must distinguish a busy, unreported replica from a missing one.
# Hold a real disk read on CS1 while its inventory is credit-gated. A write
# must preserve both current copies after that read and registration finish.

# Preserve both results: the runner otherwise returns only the last scenario's status.
# @generator
# _wrapper() {
#     local scenario failed=0
#     for scenario in release timeout; do
#         @callback@ "BUSY_DISCOVERY_MODE=$scenario"
#         if (( status != 0 )); then failed=1; fi
#     done
#     status=$failed
# }
# @endgenerator

create_bdb_name_storage_entry_() {
	echo 'USE_BDB_FOR_NAME_STORAGE = 0'
}

master_config='CHUNK_REGISTRATION_CHUNKS_PER_SECOND = 100'
master_config+='|CHUNK_REGISTRATION_BULK_SIZE = 1|CHUNK_REGISTRATION_WINDOW = 1'
master_config+='|ON_DEMAND_CHUNK_QUERY_TIMEOUT_MS = 1000'

MASTERSERVERS=2 \
	CHUNKSERVERS=2 \
	MOUNTS=2 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
	MASTER_EXTRA_CONFIG="$master_config" \
	CHUNKSERVER_EXTRA_CONFIG='MASTER_RECONNECTION_DELAY = 1|HDD_TEST_FREQ = 3600' \
	MOUNT_EXTRA_CONFIG='sfscachemode=NEVER,sfsioretries=3' \
	setup_local_empty_saunafs info

# Seed two real replicas before promotion.
target_dir="${info[mount0]}/target_dir"
mkdir "$target_dir"
saunafs setgoal 2 "$target_dir"

target_file="$target_dir/target"
FILE_SIZE=1M file-generate "$target_file"
assert_eventually_equals 'echo 2' "count_registered_parts '$target_file'"

# Preserve the original read data and the expected result of the partial write.
cp "$target_file" "$TEMP_DIR/original"
cp "$target_file" "$TEMP_DIR/expected"
dd if=/dev/zero of="$TEMP_DIR/expected" bs=4096 count=1 conv=notrunc status=none

chunk_name=$(saunafs fileinfo "$target_file" | awk '/chunk 0:/ {print $3; exit}')
chunk_id="0x${chunk_name%%_*}"
initial_version=$((16#${chunk_name##*_}))

target_data=$(find_chunkserver_chunks 1 | awk '/\.dat$/ {print}')
assert_success test -n "$target_data"

chunk_version() {
	saunafs fileinfo "$target_file" | awk '/chunk 0:/ {print $3; exit}'
}

set_write_retry_limit() {
	local retries=$1
	echo "WriteMaxRetries=$retries" | sudo tee "${info[mount0]}/.saunafs_tweaks" > /dev/null

	assert_eventually "grep -qx 'WriteMaxRetries[[:space:]]$retries' '${info[mount0]}/.saunafs_tweaks'"
}

saunafs_master_n 1 start
assert_eventually 'saunafs_shadow_synchronized 1'

# Promote with CS1 offline, leaving its replica unknown to the new master.
saunafs_chunkserver_daemon 1 stop
saunafs_master_daemon kill

saunafs_make_conf_for_shadow 0
saunafs_make_conf_for_master 1
saunafs_master_daemon reload

# Withhold inventory credits while allowing chunk-location queries through.
get_next_port_number proxy_port
prefix="$TEMP_DIR/busy-registration"

python3 "$SOURCE_DIR/tests/tools/registration_credit_gate.py" \
	--host "$(get_ip_addr)" --listen-port "$proxy_port" --master-port "${info[matocs]}" \
	--initial-credits 0 --ready-marker "$prefix-ready" --held-marker "$prefix-held" \
	--release-marker "$prefix-credits-release" --end-marker "$prefix-end" \
	--query-marker "$prefix-query" > "$prefix-proxy.log" 2>&1 &
proxy_pid=$!

reader_pid=''
writer_pid=''

cleanup_busy_registration() {
	local pid
	touch "$prefix-read-release"

	for pid in "$reader_pid" "$writer_pid" "$proxy_pid"; do
		if [[ -n "$pid" ]]; then
			kill -KILL "$pid" 2>/dev/null || true
			wait "$pid" 2>/dev/null || true
		fi
	done
}

trap cleanup_busy_registration EXIT
assert_eventually 'test -f "$prefix-ready"'

add_lines_sfschunkserver_cfg_ "MASTER_HOST = $(get_ip_addr)|MASTER_PORT = $proxy_port" 1

CHUNK_READ_GATE_TARGET="$target_data" \
	CHUNK_READ_GATE_ARMED="$prefix-read-armed" \
	CHUNK_READ_GATE_HELD="$prefix-read-held" \
	CHUNK_READ_GATE_RELEASE="$prefix-read-release" \
	LD_PRELOAD="${SAUNAFS_INSTALL_FULL_LIBDIR}/libchunk_read_gate.so" \
	saunafs_chunkserver_daemon 1 start

assert_eventually 'test -f "$prefix-held"'
assert_eventually_equals 'echo 1' "count_registered_parts '$target_file'"

# Wait for the real disk scan before arming the read gate. Only inventory
# delivery is blocked; the replica already has its chunk in the registry.
peer_pid=$(saunafs_chunkserver_daemon 1 test | awk '/pid:/ {print $NF}')
assert_eventually 'grep -Eq "chunkserver_1\[${peer_pid}\].*scanning disk .*: complete" "$ERROR_DIR/syslog.log"'

touch "$prefix-read-armed"

python3 "$SOURCE_DIR/tests/tools/registration_busy_reader.py" \
	--host "$(get_ip_addr)" --port "${info[chunkserver1_port]}" \
	--chunk-id "$chunk_id" --version "$initial_version" --expected "$TEMP_DIR/original" \
	> "$prefix-reader.log" 2>&1 &
reader_pid=$!

assert_eventually 'test -f "$prefix-read-held"'

# Exhaust retries only in the refusal scenario. The recovery scenario keeps
# its retry budget so the running client can replace stale data connections.
if [[ "$BUSY_DISCOVERY_MODE" == timeout ]]; then
	set_write_retry_limit 1
fi

(
	status=0
	dd if=/dev/zero of="$target_file" bs=4096 count=1 conv=notrunc,fsync \
		status=none > "$prefix-writer.log" 2>&1 || status=$?
	echo "$status" > "$prefix-writer-status"
) &
writer_pid=$!

# This marker observes a real query reply after the chunkserver examined the
# locked part. No artificial missing/busy response is injected by the proxy.
assert_eventually 'test -f "$prefix-query"'
assert_failure test -f "$prefix-read-release"
assert_success kill -0 "$reader_pid"
assert_failure test -f "$prefix-end"

if [[ "$BUSY_DISCOVERY_MODE" == timeout ]]; then
	# Keep the part busy until the attempt returns. Inconclusive discovery must
	# refuse a version-changing write, even though another copy is usable.
	assert_eventually 'test -f "$prefix-writer-status"' '10 seconds'

	MESSAGE='discovery of a busy replica must not acknowledge a write' \
		expect_not_equal 0 "$(cat "$prefix-writer-status")"

	MESSAGE='an unresolved busy replica must prevent a version change' \
		expect_equals "$chunk_name" "$(chunk_version)"
fi

# Release the real read and collect both background processes before checking recovery.
touch "$prefix-read-release"

reader_status=0
wait "$reader_pid" || reader_status=$?
assert_equals 0 "$reader_status"
reader_pid=''

assert_eventually 'test -f "$prefix-writer-status"' '10 seconds'

writer_status=0
wait "$writer_pid" || writer_status=$?
assert_equals 0 "$writer_status"
writer_pid=''

# After a deliberate refusal, restore retries and issue a new write on the
# same mount. Otherwise the original write must recover without intervention.
if [[ "$BUSY_DISCOVERY_MODE" == timeout ]]; then
	set_write_retry_limit 3

	assert_success dd if=/dev/zero of="$target_file" bs=4096 count=1 \
		conv=notrunc,fsync status=none
else
	MESSAGE='the running client must recover after the busy replica is released' \
		assert_equals 0 "$(cat "$prefix-writer-status")"
fi

assert_success cmp "$TEMP_DIR/expected" "$target_file"

# Finish inventory delivery and verify both replicas have the current version.
touch "$prefix-credits-release"
assert_eventually 'test -f "$prefix-end"'

saunafs fileinfo "$target_file"
cat "$prefix-reader.log" "$prefix-writer.log" "$prefix-proxy.log"

MESSAGE='the replica busy during discovery must also be current after registration' \
	expect_equals 2 "$(count_registered_parts "$target_file")"

saunafs_chunkserver_daemon 0 stop

# Mount 1 has never read the file, so cached data cannot hide a stale replica.
MESSAGE='acknowledged data must survive loss of the initially known replica' \
	assert_success cmp "$TEMP_DIR/expected" "${info[mount1]}/target_dir/target"

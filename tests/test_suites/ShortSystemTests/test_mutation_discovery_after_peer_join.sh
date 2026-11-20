timeout_set '2 minutes'

# A peer joining during discovery changes the peer-set generation. The master
# should query the new incomplete peer and keep the original write waiting.

create_bdb_name_storage_entry_() {
	echo 'USE_BDB_FOR_NAME_STORAGE = 0'
}

MASTER_EXTRA_CONFIG='ON_DEMAND_CHUNK_QUERY_TIMEOUT_MS = 10000' \
	MASTERSERVERS=2 \
	CHUNKSERVERS=1 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
	MOUNT_EXTRA_CONFIG='sfscachemode=NEVER,sfsioretries=3' \
	setup_local_empty_saunafs info

target_dir="${info[mount0]}/target_dir"
mkdir "$target_dir"
assert_success saunafs setgoal 1 "$target_dir"
target_file="$target_dir/target"
FILE_SIZE=1M file-generate "$target_file"
assert_eventually_equals 'echo 1' "count_registered_parts '$target_file'"

cp "$target_file" "$TEMP_DIR/original"
cp "$target_file" "$TEMP_DIR/expected"
dd if=/dev/zero of="$TEMP_DIR/expected" bs=4096 count=1 conv=notrunc status=none

# Promote a synchronized shadow so the existing chunk needs a version change.
saunafs_master_n 1 start
assert_eventually 'saunafs_shadow_synchronized 1'
saunafs_master_daemon kill
saunafs_make_conf_for_shadow 0
saunafs_make_conf_for_master 1
saunafs_master_daemon reload
assert_eventually_equals 'echo 1' "count_registered_parts '$target_file'"

# Route the mount through a packet observer to detect any reply to the write.
get_next_port_number client_proxy_port
client_prefix="$TEMP_DIR/client"
python3 "$SOURCE_DIR/tests/tools/registration_truncate_client_proxy.py" \
	--host "$(get_ip_addr)" --listen-port "$client_proxy_port" \
	--master-port "${info[matocl]}" --prefix "$client_prefix" \
	> "$TEMP_DIR/client-proxy.log" 2>&1 &
client_proxy_pid=$!
echo "sfsport=$client_proxy_port" >> "${info[mount0_cfg]}"
assert_eventually 'test -f "$client_prefix-ready"'
saunafs_mount_unmount 0
saunafs_mount_start 0

peer_a="$TEMP_DIR/peer-a"
peer_b="$TEMP_DIR/peer-b"
peer_pids=()
writer_pid=''
cleanup_peer_join_test() {
	local pid
	touch "$peer_a-release" "$peer_b-release"
	for pid in "${peer_pids[@]}" "$client_proxy_pid" "$writer_pid"; do
		if [[ -n "$pid" ]]; then
			kill "$pid" 2>/dev/null || true
			wait "$pid" 2>/dev/null || true
		fi
	done
}
trap cleanup_peer_join_test EXIT

start_incomplete_peer() {
	local prefix=$1 service_port
	shift
	get_next_port_number service_port
	python3 "$SOURCE_DIR/tests/tools/registration_incomplete_peer.py" \
		--host "$(get_ip_addr)" --master-port "${info[matocs]}" \
		--service-port "$service_port" --prefix "$prefix" "$@" > "$prefix.log" 2>&1 &
	peer_pids+=("$!")
	assert_eventually "test -f '$prefix-ready'"
}

start_incomplete_peer "$peer_a"

dd if=/dev/zero of="$target_file" bs=4096 count=1 conv=notrunc,fsync \
	> "$TEMP_DIR/write.log" 2>&1 &
writer_pid=$!

# Peer A receives the first discovery query and holds its reply.
assert_eventually 'test -f "$peer_a-query"'
sleep 1 # Let the query fan-out finish before changing the peer set.

# Peer B joins while the query is pending. Its reply is held separately from
# its inventory end so the test can observe the generation-change retry.
start_incomplete_peer "$peer_b" \
	--query-release-marker "$peer_b-query-release"
assert_eventually 'test -f "$peer_b-query"'
touch "$peer_a-release"
assert_eventually 'test -f "$peer_a-completed"'
sleep 1 # Allow the master to process A's reply while B is still unanswered.

# The master must keep waiting for B rather than returning A's stale result.
MESSAGE='the original write must remain pending while B has not replied' \
	expect_failure test -f "$client_prefix-write-status"
MESSAGE='the mount must not resend the write while discovery is pending' \
	assert_success kill -0 "$writer_pid"
	assert_equals 1 "$(cat "$client_prefix-write-request-count")"

# B answers the original query but stays incomplete. Because it joined after
# the first query, the master must query the current peer set again.
touch "$peer_b-query-release"
assert_eventually 'test -f "$peer_b-queries-released"'
assert_eventually 'test -f "$peer_b-query-2"' '5 seconds'
MESSAGE='the retry must stay inside the original client request' \
	assert_equals 1 "$(cat "$client_prefix-write-request-count")"
assert_eventually 'test -f "$client_prefix-write-status"' '15 seconds'
wait "$writer_pid"

MESSAGE='the write should succeed after the new peer answers discovery' \
	expect_equals 0 "$(cat "$client_prefix-write-status")"
assert_equals 1 "$(cat "$client_prefix-write-request-count")"
expect_files_equal "$TEMP_DIR/expected" "$target_file"

cat "$TEMP_DIR/write.log" "$TEMP_DIR/client-proxy.log" "$peer_a.log" "$peer_b.log"

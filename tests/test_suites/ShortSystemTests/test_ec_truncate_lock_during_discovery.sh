timeout_set '2 minutes'

# Introduce an incomplete inventory after the truncate lock was granted. If END
# waits for discovery, another mount must not acquire that chunk in the meantime.

source "$SOURCE_DIR/tests/tools/ec_truncate_registration.sh"
EC_TRUNCATE_QUERY_TIMEOUT_MS=10000
ec_truncate_setup

ec_truncate_trace_mount 0 "$TEMP_DIR/client" hold
ec_truncate_trace_mount 1 "$TEMP_DIR/writer"
ec_truncate_start_operation

# Pause before zeroing, then change the peer set between the two truncate phases.
assert_eventually 'test -f "$TEMP_DIR/client-prepared"'
ec_truncate_start_peer
touch "$TEMP_DIR/prepared-release"

assert_eventually 'test -f "$TEMP_DIR/client-end-sent"'
assert_eventually 'test -f "$TEMP_DIR/peer-query" || test -f "$TEMP_DIR/truncate-result"'

writer_started=0
if [[ ! -f "$TEMP_DIR/truncate-result" ]]; then
	writer_started=1
	(
		result=0
		dd if=/dev/zero of="${info[mount1]}/ec21/target" bs=4096 count=1 \
			conv=notrunc,fsync status=none > "$TEMP_DIR/concurrent-write.log" 2>&1 || result=$?
		echo "$result" > "$TEMP_DIR/writer-result.tmp"
		mv "$TEMP_DIR/writer-result.tmp" "$TEMP_DIR/writer-result"
	) &
	writer_pid=$!
	ec_truncate_pids+=("$writer_pid")

	# Observe the actual write-start reply, rather than inferring a lock from sleep.
	assert_eventually 'test -f "$TEMP_DIR/writer-write-status" || test -f "$TEMP_DIR/truncate-result"'
	if [[ ! -f "$TEMP_DIR/truncate-result" ]]; then
		MESSAGE='TRUNCATE_END must retain ownership while discovery is unresolved' \
			expect_equals 11 "$(cat "$TEMP_DIR/writer-write-status")"
	fi
fi

ec_truncate_release_peer
assert_eventually 'test -f "$TEMP_DIR/truncate-result"' '15 seconds'
wait "$truncate_pid"

MESSAGE='a granted client-assisted truncate must complete despite the new incomplete peer' \
	expect_equals 0 "$(cat "$TEMP_DIR/truncate-result")"

cp "$TEMP_DIR/truncated" "$TEMP_DIR/expected"
if (( writer_started )); then
	assert_eventually 'test -f "$TEMP_DIR/writer-result"' '15 seconds'
	wait "$writer_pid"
	expect_equals 0 "$(cat "$TEMP_DIR/writer-result")"
	dd if=/dev/zero of="$TEMP_DIR/expected" bs=4096 count=1 conv=notrunc status=none
	cat "$TEMP_DIR/concurrent-write.log"
fi

expect_files_equal "$TEMP_DIR/expected" "$verification_file"
cat "$TEMP_DIR/truncate.log" "$TEMP_DIR/client.log" "$TEMP_DIR/writer.log" "$TEMP_DIR/peer.log"

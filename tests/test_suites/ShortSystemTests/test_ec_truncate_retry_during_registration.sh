timeout_set '2 minutes'

# After a master restart, the first truncate needs version discovery. A temporary
# refusal must use a status the running mount retries, before it zeroes any data.

source "$SOURCE_DIR/tests/tools/ec_truncate_registration.sh"
ec_truncate_setup

saunafs_master_daemon restart
assert_eventually_equals 'echo 3' "count_registered_parts '$target_file'"

ec_truncate_trace_mount 0 "$TEMP_DIR/client"
ec_truncate_start_peer
ec_truncate_start_operation

assert_eventually 'test -f "$TEMP_DIR/peer-query"'
assert_eventually 'test -f "$TEMP_DIR/client-truncate-status"' '10 seconds'

first_status=$(cat "$TEMP_DIR/client-truncate-status")
echo "INITIAL_TRUNCATE_STATUS: $first_status"

# These are the existing mount's retryable truncate statuses: CHUNKLOST,
# LOCKED and NOTDONE. NOCHUNKSERVERS would escape to the application as EINVAL.
MESSAGE='temporary truncate admission failure must be retryable by existing mounts' \
	expect_matches '^(8|11|16)$' "$first_status"

MESSAGE='initial discovery refusal must precede client-side zeroing' \
	expect_failure test -f "$TEMP_DIR/client-prepared"
expect_files_equal "$TEMP_DIR/original" "$verification_file"

# No new syscall or mount restart: release inventory and let the original retry finish.
ec_truncate_release_peer
assert_eventually 'test -f "$TEMP_DIR/truncate-result"' '15 seconds'
wait "$truncate_pid"
cat "$TEMP_DIR/truncate.log" "$TEMP_DIR/client.log" "$TEMP_DIR/peer.log"

MESSAGE='the original truncate must recover after the temporary refusal' \
	expect_equals 0 "$(cat "$TEMP_DIR/truncate-result")"
# The pre-refusal observer can still cache the original size after a remote truncate.
expect_files_equal "$TEMP_DIR/truncated" "${info[mount1]}/ec21/target"

timeout_set '2 minutes'

# A refused partial EC truncate must preserve the entire file, including the
# tail the mount would zero before TRUNCATE_END. Retry after inventory recovery.

# @generator
# _wrapper() {
#     local scenario failed=0
#     for scenario in timeout disabled legacy; do
#         @callback@ "EC_TRUNCATE_FAILURE_MODE=$scenario"
#         if (( status != 0 )); then failed=1; fi
#     done
#     status=$failed
# }
# @endgenerator

source "$SOURCE_DIR/tests/tools/ec_truncate_registration.sh"

if [[ "$EC_TRUNCATE_FAILURE_MODE" == disabled ]]; then
	EC_TRUNCATE_QUERY_LIMIT=0
fi

ec_truncate_setup
ec_truncate_trace_mount 0 "$TEMP_DIR/client"
ec_truncate_start_peer "$EC_TRUNCATE_FAILURE_MODE"
ec_truncate_start_operation

# Keep confirmation unavailable until the client's bounded attempt returns.
assert_eventually 'test -f "$TEMP_DIR/truncate-result"' '15 seconds'
wait "$truncate_pid"
cat "$TEMP_DIR/truncate.log" "$TEMP_DIR/client.log" "$TEMP_DIR/peer.log"

MESSAGE='unresolved discovery must not acknowledge the truncate' \
	expect_not_equal 0 "$(cat "$TEMP_DIR/truncate-result")"
MESSAGE='a refused truncate must retain the original file length' \
	expect_equals 1048576 "$(stat -c %s "$verification_file")"
MESSAGE='a refused truncate must retain every byte, including the original tail' \
	expect_files_equal "$TEMP_DIR/original" "$verification_file"

# The same running mount must succeed once registration is complete.
ec_truncate_release_peer
assert_success truncate -s "$truncate_length" "$target_file"
# Mount 2 checked the old file; use an untouched observer to avoid its cached size.
assert_files_equal "$TEMP_DIR/truncated" "${info[mount1]}/ec21/target"

echo "EC_TRUNCATE_REFUSAL_CHECKED: $EC_TRUNCATE_FAILURE_MODE"

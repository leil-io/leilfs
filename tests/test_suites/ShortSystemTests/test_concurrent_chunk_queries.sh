timeout_set '5 minutes'

# Verify concurrent reads across mounts resolve chunk locations on demand
# during paced registration, including multiple waiters for one shared chunk.
#
# This tests query concurrency, not the packet-size limit: event-loop flushing
# and per-mount request serialization keep batches small.
# MatocsservChunkQuerySplitTests in src/master/matocsserv_unittest.cc
# covers packet-size boundaries directly.

CHUNK_COUNT=${CHUNK_COUNT:-600}
CHUNKS_PER_FILE=1
READERS_PER_MOUNT=${READERS_PER_MOUNT:-100}
SHARED_READERS_PER_MOUNT=${SHARED_READERS_PER_MOUNT:-20}
MOUNT_COUNT=3
REGISTRATION_CHUNKS_PER_SECOND=50

MASTERSERVERS=2 \
	CHUNKSERVERS=1 \
	MOUNTS=$MOUNT_COUNT \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
	MASTER_EXTRA_CONFIG="CHUNK_REGISTRATION_CHUNKS_PER_SECOND = ${REGISTRATION_CHUNKS_PER_SECOND}`
			`|CHUNK_REGISTRATION_BULK_SIZE = 1`
			`|ON_DEMAND_CHUNK_QUERY_TIMEOUT_MS = 30000" \
	CHUNKSERVER_EXTRA_CONFIG="MASTER_RECONNECTION_DELAY = 1" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

syslog_file="${ERROR_DIR}/syslog.log"

# Seed one mock chunk per file offline for distinct chunk-location queries.
saunafs_master_daemon stop
metadata_version=$(metadata_get_version "${info[master_data_path]}/metadata.sfs")
generate_mock_chunks_changelog "$CHUNK_COUNT" "$CHUNKS_PER_FILE" "$metadata_version" \
	> "${info[master_data_path]}/changelog.sfs"
assert_success sfsmetarestore -a -d "${info[master_data_path]}"
saunafs_master_daemon start
saunafs_wait_for_all_ready_chunkservers

mock_dir="$RAMDISK_DIR/mock_disk_0"
mkdir -p "$mock_dir"
echo "mock:${CHUNK_COUNT}:1:${mock_dir}" >> "${info[chunkserver0_hdd]}"
saunafs_chunkserver_daemon 0 reload

assert_eventually_equals "echo $CHUNK_COUNT" 'count_chunk_copies'
echo "PHASE: $CHUNK_COUNT chunks registered"

saunafs_master_n 1 start
assert_eventually "saunafs_shadow_synchronized 1"

# Promote the shadow and pace re-registration to leave locations unresolved.
saunafs_master_daemon kill
saunafs_make_conf_for_shadow 0
saunafs_make_conf_for_master 1
saunafs_master_daemon reload

registration_in_progress() {
	local copies
	copies=$(count_chunk_copies)
	[[ -n "$copies" && "$copies" -gt 0 && "$copies" -lt "$CHUNK_COUNT" ]]
}
assert_eventually 'registration_in_progress'
echo "PHASE: promoted, re-registration running ($(count_chunk_copies) / $CHUNK_COUNT known)"

# Query distinct chunks alongside a shared chunk whose waiters must all wake.
# Save each shared read's result to detect waiters that never receive data.
shared_idx=$((CHUNK_COUNT - 1))
shared_file=$(printf 'mock_%07d' "$shared_idx")
distinct_results="$TEMP_DIR/distinct_chunk_reads"
shared_results="$TEMP_DIR/shared_chunk_reads"
rm -rf "$distinct_results"
rm -rf "$shared_results"
mkdir -p "$distinct_results"
mkdir -p "$shared_results"

reader_pids=()
for ((m = 0; m < MOUNT_COUNT; ++m)); do
	(
		for ((r = 0; r < READERS_PER_MOUNT; ++r)); do
			idx=$((m * READERS_PER_MOUNT + r))
			[[ $idx -lt $shared_idx ]] || break
			dd if="${info[mount$m]}/$(printf 'mock_%07d' "$idx")" bs=16 count=1 \
				2>/dev/null | od -An -tx1 | tr -d ' \n' > "$distinct_results/${m}_${r}" &
		done
		for ((s = 0; s < SHARED_READERS_PER_MOUNT; ++s)); do
			dd if="${info[mount$m]}/$shared_file" bs=16 count=1 2>/dev/null \
				| od -An -tx1 | tr -d ' \n' > "$shared_results/${m}_${s}" &
		done
		wait
	) &
	reader_pids+=($!)
done
for pid in "${reader_pids[@]}"; do wait "$pid"; done
echo "PHASE: burst done, $((MOUNT_COUNT * READERS_PER_MOUNT)) reads on distinct chunks," \
	"$((MOUNT_COUNT * SHARED_READERS_PER_MOUNT)) on chunk $shared_idx"

expected_head="5a5b58595e5f5c5d5253505156575455"

# Check result count as well as contents: checking only existing files
# could hide missing readers.
distinct_expected=$((MOUNT_COUNT * READERS_PER_MOUNT))
if ((distinct_expected > shared_idx)); then distinct_expected=$shared_idx; fi
MESSAGE="every reader on a distinct chunk must have produced a result" \
	assert_equals "$distinct_expected" "$(find "$distinct_results" -type f | wc -l)"
distinct_correct=$(grep -lFx "$expected_head" "$distinct_results"/* 2>/dev/null | wc -l)
MESSAGE="every distinct chunk must read back after on-demand resolution" \
	assert_equals "$distinct_expected" "$distinct_correct"

shared_expected=$((MOUNT_COUNT * SHARED_READERS_PER_MOUNT))
MESSAGE="every reader on the shared chunk must have produced a result" \
	assert_equals "$shared_expected" "$(find "$shared_results" -type f | wc -l)"
shared_correct=$(grep -lFx "$expected_head" "$shared_results"/* 2>/dev/null | wc -l)
MESSAGE="every waiter on one chunk must be woken by its single resolution" \
	assert_equals "$shared_expected" "$shared_correct"

# Verify a representative distinct-chunk read on each mount.
for ((m = 0; m < MOUNT_COUNT; ++m)); do
	idx=$((m * READERS_PER_MOUNT))
	MESSAGE="chunk $idx must read back after being resolved on demand" \
		assert_equals "$expected_head" \
		"$(dd if="${info[mount$m]}/$(printf 'mock_%07d' "$idx")" bs=16 count=1 2>/dev/null \
			| od -An -tx1 | tr -d ' \n')"
done

# Match 'packet <type> too long', the exception text actually logged.
# The exception class name is not present in syslog.
MESSAGE="no peer may be dropped for an oversized packet while queries are answered" \
	assert_equals 0 "$(grep -cE 'packet [0-9]+ too long' "$syslog_file" | cat)"

# Require full registration convergence.
assert_eventually_equals "echo $CHUNK_COUNT" 'count_chunk_copies' '3 minutes'

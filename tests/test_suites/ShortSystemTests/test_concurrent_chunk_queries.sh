timeout_set '5 minutes'

# Concurrent readers all blocking on distinct not-yet-registered chunks: the
# master must resolve every one of them on demand and answer correctly, with
# several mounts querying at the same time.
#
# This does NOT cover the packet limit that the query batching exists for.
# A chunkserver drops the connection on a master packet above 10000 bytes
# (kMaxMasterToChunkserverPacketSize), which at 8 bytes an id means a batch
# needs ~1248 chunks blocked between two event-loop iterations to trigger.
# The loop flushes constantly and a mount serialises its own chunk requests,
# so the ceiling here is set by mount count, not by reader processes: measured
# on this setup with 1600 concurrent readers across 4 mounts, the largest batch
# the master ever built was 6 ids. Reaching 1248 takes a cluster's worth of
# client connections, which no system test on one machine can stand up.
#
# The limit itself is covered by MatocsservChunkQuerySplitTests in
# src/master/matocsserv_unittest.cc, which drives the batching directly and
# fails when a packet exceeds what a chunkserver accepts.

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

# Seed metadata offline so the chunks exist without being written through a
# mount: one chunk per file, so every read blocks on a different chunk.
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

# Promote: the chunkserver reconnects and re-registers slowly enough that the
# readers below all block on chunks whose location the master does not know.
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

# The burst: every reader touches a distinct chunk, so each one the master
# cannot place has to be resolved on its own.
#
# Alongside it, a second group all reading the SAME chunk. The master keeps one
# query per chunk and parks the rest of the waiters on it, so these resolve
# together off a single answer rather than each asking again. Their results are
# kept: a resolution that woke only the first waiter would leave the others
# reading nothing, which nothing else here would notice.
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

# Every reader on a distinct chunk must have got its data. The reader jobs
# write their own results because wait only reports process completion, not
# whether every read succeeded.
distinct_expected=$((MOUNT_COUNT * READERS_PER_MOUNT))
if ((distinct_expected > shared_idx)); then distinct_expected=$shared_idx; fi
MESSAGE="every reader on a distinct chunk must have produced a result" \
	assert_equals "$distinct_expected" "$(find "$distinct_results" -type f | wc -l)"
distinct_correct=$(grep -lFx "$expected_head" "$distinct_results"/* 2>/dev/null | wc -l)
MESSAGE="every distinct chunk must read back after on-demand resolution" \
	assert_equals "$distinct_expected" "$distinct_correct"

# Every waiter parked on the shared chunk got the answer, not just whichever
# one caused the query. Counted as well as compared: a reader that never ran
# leaves no file behind, and comparing only the files that exist would pass
# while most of them were missing.
shared_expected=$((MOUNT_COUNT * SHARED_READERS_PER_MOUNT))
MESSAGE="every reader on the shared chunk must have produced a result" \
	assert_equals "$shared_expected" "$(find "$shared_results" -type f | wc -l)"
shared_correct=$(grep -lFx "$expected_head" "$shared_results"/* 2>/dev/null | wc -l)
MESSAGE="every waiter on one chunk must be woken by its single resolution" \
	assert_equals "$shared_expected" "$shared_correct"

# Every reader must have got its data, not an error: each was blocked on a
# chunk whose location only the on-demand query could supply.
for ((m = 0; m < MOUNT_COUNT; ++m)); do
	idx=$((m * READERS_PER_MOUNT))
	MESSAGE="chunk $idx must read back after being resolved on demand" \
		assert_equals "$expected_head" \
		"$(dd if="${info[mount$m]}/$(printf 'mock_%07d' "$idx")" bs=16 count=1 2>/dev/null \
			| od -An -tx1 | tr -d ' \n')"
done

# The queries must not have cost anyone their connection. Matched on the text
# the exception actually carries: "packet <type> too long (<size>/<limit>)",
# (see InputPacket::increaseBytesRead) since only ex.what() reaches the log:
# the class name never appears there, and the packet type sits between "packet"
# and "too long", so a pattern built from either would never match at all.
MESSAGE="no peer may be dropped for an oversized packet while queries are answered" \
	assert_equals 0 "$(grep -cE 'packet [0-9]+ too long' "$syslog_file" | cat)"

# And everything still converges
assert_eventually_equals "echo $CHUNK_COUNT" 'count_chunk_copies' '3 minutes'

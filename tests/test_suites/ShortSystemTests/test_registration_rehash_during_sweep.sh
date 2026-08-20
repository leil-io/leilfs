timeout_set '4 minutes'

# The resumable pull-registration sweep must not lose chunks when the registry
# REHASHES underneath it. The sweep walks gChunksMap bucket by bucket, keeping
# a cursor between calls; a rehash redistributes every entry across a new
# bucket array, so chunks the cursor has not reached yet can land behind it and
# be skipped. Catching them is the whole job of the sweep's termination pass,
# and nothing else in the suite exercises that path.
#
# Companion to test_registration_with_concurrent_mutations, which covers the
# other half: chunks INSERTED mid-sweep, at volume. That test cannot cover the
# rehash, and the reason is why this one exists separately. The synthetic scan
# reserves gChunksMap for the whole seeded registry up front
# (SyntheticChunkSink::reserve), so with a large seed no realistic number of
# concurrent inserts ever crosses the load factor.
#
# The trick here is the inverted ratio: seed a deliberately TINY registry so
# the reserve is small, then create many times that many files while the sweep
# runs. reserve(N) leaves the map just under its load factor: 100 chunks in
# 103 buckets, so the inserts begin rehashing almost at once.
# NEW_FILE_COUNT being several times CHUNK_COUNT then spreads repeated rehashes
# across the whole insert window, rather than staking the test on a single one
# landing at a useful moment.
#
# The rehash is confirmed, not assumed: the chunkserver reports its bucket
# count when the sweep starts and again when it completes, and the two must
# differ. Running with NEW_FILE_COUNT=0 makes that assertion fail, which is
# what shows it still has teeth.
#
# The window is opened by promoting a shadow, not by restarting the
# chunkserver, and that is load-bearing. The rehash has to happen in the
# registry that is being swept, so the sweeping chunkserver must be the one
# accepting the new chunks. A restarted chunkserver advertises no free space
# until its registration has COMPLETED, so the leader would never place them
# there; a reconnecting one sends its registration tail upfront and takes new
# chunks immediately, while its sweep is still running.

CHUNK_COUNT=${CHUNK_COUNT:-100}
CHUNKS_PER_FILE=10
NEW_FILE_COUNT=${NEW_FILE_COUNT:-600}
# Paced for CHUNK_COUNT + NEW_FILE_COUNT, not for the seeded count alone: the
# chunks created below are unmarked in the registry, so the sweep has to
# deliver them as well and its window is set by the total. Slow enough that the
# seeded chunks alone take several seconds, so the inserts cannot finish before
# the sweep has begun walking.
REGISTRATION_CHUNKS_PER_SECOND=20

MASTERSERVERS=2 \
	CHUNKSERVERS=1 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
	MASTER_EXTRA_CONFIG="CHUNK_REGISTRATION_CHUNKS_PER_SECOND = ${REGISTRATION_CHUNKS_PER_SECOND}`
			`|CHUNK_REGISTRATION_BULK_SIZE = 1" \
	CHUNKSERVER_EXTRA_CONFIG="MASTER_RECONNECTION_DELAY = 1" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

syslog_file="${ERROR_DIR}/syslog.log"

# Seed metadata offline, then let the mock disk synthesize the matching chunks
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
echo "PHASE: $CHUNK_COUNT mock chunks registered, registry reserved for that many"

# Bring up the shadow and let it catch up
saunafs_master_n 1 start
assert_eventually "saunafs_shadow_synchronized 1"

# The start and completion messages belong to the chunkserver, not the master.
# Before promotion, wait until all earlier registration messages have reached
# syslog and retain their settled counts as the baseline for the promoted sweep.
registration_started_count() {
	grep -c "master-driven chunk registration started" "$syslog_file" | cat
}
registration_complete_count() {
	grep -c "master-driven chunk registration complete" "$syslog_file" | cat
}
registration_log_baseline_settled() {
	local started complete
	started=$(registration_started_count)
	complete=$(registration_complete_count)
	[[ "$started" -gt 0 && "$started" -eq "$complete" ]]
}
MESSAGE="the pre-promotion registration logs must be settled" \
	assert_eventually 'registration_log_baseline_settled'
registration_starts_before_promotion=$(registration_started_count)
registration_completes_before_promotion=$(registration_complete_count)

# Sweeps completed by the master promoted below. Before the promotion this
# master has no completed registrations, so this confirms the current sweep
# reached its end as well as the chunkserver's two local messages.
sweeps_finished_after_promotion() {
	grep -c "master_1\[.*finished master-driven chunk registration" "$syslog_file" | cat
}

# Bucket counts at the two ends of the LAST sweep, which is the one opened by
# the promotion below. Taking the last of each matters: this log also holds the
# registrations from before the mock disk existed, against an empty registry of
# one bucket. Comparing one sweep's start against another's completion would
# report the registry merely growing as though it were a rehash.
registration_started_buckets() {
	grep -oE "master-driven chunk registration started \(.*registry buckets [0-9]+" \
		"$syslog_file" | grep -oE '[0-9]+$' | tail -1
}
registration_complete_buckets() {
	grep -oE "master-driven chunk registration complete \(.*registry buckets [0-9]+" \
		"$syslog_file" | grep -oE '[0-9]+$' | tail -1
}

# Both local ends of the promoted sweep must be on record, beyond the settled
# baseline. The master completion prevents delayed pre-promotion chunkserver
# logs from being mistaken for the current sweep.
sweep_ends_logged() {
	local started complete finished
	started=$(registration_started_count)
	complete=$(registration_complete_count)
	finished=$(sweeps_finished_after_promotion)
	[[ "$finished" -gt 0 &&
		"$started" -gt "$registration_starts_before_promotion" &&
		"$complete" -gt "$registration_completes_before_promotion" &&
		"$started" -eq "$complete" ]]
}

# Promote the shadow: the chunkserver reconnects and re-registers, paced slowly
# enough that the file creation below fits comfortably inside the sweep.
saunafs_master_daemon kill
saunafs_make_conf_for_shadow 0
saunafs_make_conf_for_master 1
saunafs_master_daemon reload

# Wait only until the sweep has STARTED. Deliberately not
# saunafs_wait_for_all_ready_chunkservers: readiness is the post-registration
# space report, so waiting for it would close the very window needed here.
registration_in_progress() {
	local copies
	copies=$(count_chunk_copies)
	[[ -n "$copies" && "$copies" -gt 0 && "$copies" -lt "$CHUNK_COUNT" ]]
}
assert_eventually 'registration_in_progress'
echo "PHASE: promoted, paced re-registration running" \
	"($(count_chunk_copies) / $CHUNK_COUNT copies known)"

# The inserts: enough new chunks to push the registry past the capacity
# reserved for the seeded ones, forcing the rehash this test is about.
create_start_ns=$(date +%s%N)
for ((i = 0; i < NEW_FILE_COUNT; ++i)); do
	echo "rehash $i" > "${info[mount0]}/$(printf 'rehash_%05d' "$i")"
done
create_ms=$(( ($(date +%s%N) - create_start_ns) / 1000000 ))
echo "REHASH_INSERT_MS: $create_ms for $NEW_FILE_COUNT files"

# The inserts must have landed while the sweep was still walking the registry,
# otherwise they rehashed a map nobody was iterating and this test measured
# nothing.
MESSAGE="inserts must land in a registry that is still being swept" \
	assert_equals 0 "$(sweeps_finished_after_promotion)"

# Convergence: every seeded chunk plus every new one is known to the master.
# This is the assertion that would catch a chunk lost to the rehash: the
# bucket-count check below only proves the hazard was present.
#
# All of the detection lives in the CHUNK_COUNT seeded chunks, not in the total.
# The master ordered the rehash_* chunks itself, so it knows which chunkserver
# holds them whatever the sweep does: drop every one of them from the sweep
# and this count would still be reached. The seeded chunks it knows only from
# metadata, and after the promotion the sweep is the sole channel that says
# where they are: the new-chunk queue was drained before the promotion and is
# empty. Lowering CHUNK_COUNT therefore weakens this test far more than the
# copy total suggests.
expected_copies=$((CHUNK_COUNT + NEW_FILE_COUNT))
copies_at_least() {
	local copies
	copies=$(count_chunk_copies)
	[[ -n "$copies" && "$copies" -ge "$expected_copies" ]]
}
assert_eventually 'copies_at_least' '2 minutes'
echo "PHASE: converged at $(count_chunk_copies) / $expected_copies copies"

# The hazard must actually have occurred: the registry the sweep started on is
# not the one it finished on. Without this the test could pass on a registry
# that never rehashed, proving nothing about the termination pass.
# Convergence does not mean the sweep is over: the master already knew the
# chunks it ordered created, so it reaches the full count long before the sweep
# has streamed everything at its budget.
MESSAGE="the sweep must have reported both of its ends before they are compared" \
	assert_eventually 'sweep_ends_logged' '2 minutes'
buckets_at_start=$(registration_started_buckets)
buckets_at_end=$(registration_complete_buckets)
echo "REHASH_BUCKETS: $buckets_at_start at sweep start, $buckets_at_end at completion"
MESSAGE="the registry must have rehashed while the sweep was walking it" \
	assert_less_than "$buckets_at_start" "$buckets_at_end"

# Every seeded chunk read back through the mount. Counting copies proves the
# master tallied them; reading proves the location it hands out is usable,
# which a copy recorded at the wrong version or against the wrong server would
# not be. Each of these chunks reached the master only by surviving the sweep,
# so this covers exactly the population the rehash put at risk: one block per
# chunk rather than whole files, which at 64 MiB each would be gigabytes.
SFS_CHUNK_SIZE=67108864
# Every 64KiB block of a mock chunk holds the static pattern
# byte[i] = (i & 0xFF) ^ 0x5A, so one expected value serves every offset.
expected_head="5a5b58595e5f5c5d5253505156575455"
read_block() {
	dd if="$1" bs=16 count=1 skip="${2:-0}" iflag=skip_bytes 2>/dev/null \
		| od -An -tx1 | tr -d ' \n'
}
verified_chunks=0
for ((f = 0; f < CHUNK_COUNT / CHUNKS_PER_FILE; ++f)); do
	mock_file="${info[mount0]}/$(printf 'mock_%07d' "$f")"
	for ((c = 0; c < CHUNKS_PER_FILE; ++c)); do
		MESSAGE="chunk $c of $(printf 'mock_%07d' "$f") must be readable after the rehash" \
			assert_equals "$expected_head" "$(read_block "$mock_file" $((c * SFS_CHUNK_SIZE)))"
		verified_chunks=$((verified_chunks + 1))
	done
done
# Reported so a loop that silently stops iterating cannot pass unnoticed
MESSAGE="every seeded chunk must have been read back" \
	assert_equals "$CHUNK_COUNT" "$verified_chunks"
echo "REHASH_VERIFIED_CHUNKS: $verified_chunks seeded chunks read back"

# The new chunks are a data-integrity smoke check only: the master knew them
# without the sweep, so they cannot detect a chunk lost behind the cursor.
assert_equals "rehash 0" "$(cat "${info[mount0]}/rehash_00000")"
assert_equals "rehash $((NEW_FILE_COUNT - 1))" \
	"$(cat "${info[mount0]}/$(printf 'rehash_%05d' $((NEW_FILE_COUNT - 1)))")"

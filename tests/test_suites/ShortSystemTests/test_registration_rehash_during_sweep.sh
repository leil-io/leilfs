timeout_set '4 minutes'

# Verify pull registration does not lose chunks when a registry rehash moves
# entries behind the sweep cursor. Seed a small registry, then create enough
# files during the paced sweep to exceed its reserved capacity.
#
# Unlike test_registration_with_concurrent_mutations, the small seed makes
# rehashing practical. Confirm bucket growth and read back every seeded chunk.
#
# Promote a shadow to reuse already-scanned disks and advertise free space
# before the sweep. A restart may initially report zero space while scanning.

CHUNK_COUNT=${CHUNK_COUNT:-100}
CHUNKS_PER_FILE=10
NEW_FILE_COUNT=${NEW_FILE_COUNT:-600}
# Leave time for inserts during the sweep, which must report both seeded
# and newly created chunks.
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

# Wait for earlier chunkserver registration logs to settle before recording
# the baseline for the promoted sweep.
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

# The promoted master's first completion confirms the current sweep ended.
sweeps_finished_after_promotion() {
	grep -c "master_1\[.*finished master-driven chunk registration" "$syslog_file" | cat
}

# Use the last sweep's bucket counts; mixing earlier and current sweeps
# could mistake registry growth between registrations for a mid-sweep rehash.
registration_started_buckets() {
	grep -oE "master-driven chunk registration started \(.*registry buckets [0-9]+" \
		"$syslog_file" | grep -oE '[0-9]+$' | tail -1
}
registration_complete_buckets() {
	grep -oE "master-driven chunk registration complete \(.*registry buckets [0-9]+" \
		"$syslog_file" | grep -oE '[0-9]+$' | tail -1
}

# Require new start/end logs and master completion to exclude delayed logs
# from registrations before promotion.
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

# Promote the shadow to start paced re-registration.
saunafs_master_daemon kill
saunafs_make_conf_for_shadow 0
saunafs_make_conf_for_master 1
saunafs_master_daemon reload

# Wait for partial registration, not full readiness, to preserve the
# mutation window.
registration_in_progress() {
	local copies
	copies=$(count_chunk_copies)
	[[ -n "$copies" && "$copies" -gt 0 && "$copies" -lt "$CHUNK_COUNT" ]]
}
assert_eventually 'registration_in_progress'
echo "PHASE: promoted, paced re-registration running" \
	"($(count_chunk_copies) / $CHUNK_COUNT copies known)"

# Exceed the registry's reserved capacity to force rehashing during the sweep.
create_start_ns=$(date +%s%N)
for ((i = 0; i < NEW_FILE_COUNT; ++i)); do
	echo "rehash $i" > "${info[mount0]}/$(printf 'rehash_%05d' "$i")"
done
create_ms=$(( ($(date +%s%N) - create_start_ns) / 1000000 ))
echo "REHASH_INSERT_MS: $create_ms for $NEW_FILE_COUNT files"

# Require inserts before sweep completion to prove the rehash was concurrent.
MESSAGE="inserts must land in a registry that is still being swept" \
	assert_equals 0 "$(sweeps_finished_after_promotion)"

# Require all seeded and new chunks. Only seeded locations depend on the
# sweep: the master already knows where it ordered the new chunks created.
# Reducing CHUNK_COUNT weakens lost-chunk detection even with many new files.
expected_copies=$((CHUNK_COUNT + NEW_FILE_COUNT))
copies_at_least() {
	local copies
	copies=$(count_chunk_copies)
	[[ -n "$copies" && "$copies" -ge "$expected_copies" ]]
}
assert_eventually 'copies_at_least' '2 minutes'
echo "PHASE: converged at $(count_chunk_copies) / $expected_copies copies"

# Convergence can precede sweep completion because new locations are known.
# Wait for both ends, then require bucket growth to prove a rehash occurred.
MESSAGE="the sweep must have reported both of its ends before they are compared" \
	assert_eventually 'sweep_ends_logged' '2 minutes'
buckets_at_start=$(registration_started_buckets)
buckets_at_end=$(registration_complete_buckets)
echo "REHASH_BUCKETS: $buckets_at_start at sweep start, $buckets_at_end at completion"
MESSAGE="the registry must have rehashed while the sweep was walking it" \
	assert_less_than "$buckets_at_start" "$buckets_at_end"

# Read 16 bytes from every seeded chunk to verify usable registered locations
# and correct data without reading entire 64 MiB chunks.
SFS_CHUNK_SIZE=67108864
# Mock blocks repeat byte[i] = (i & 0xFF) ^ 0x5A at every probed offset.
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
# Ensure the read-back loop covered every seeded chunk.
MESSAGE="every seeded chunk must have been read back" \
	assert_equals "$CHUNK_COUNT" "$verified_chunks"
echo "REHASH_VERIFIED_CHUNKS: $verified_chunks seeded chunks read back"

# New-file reads check data integrity, not recovery of locations by the sweep.
assert_equals "rehash 0" "$(cat "${info[mount0]}/rehash_00000")"
assert_equals "rehash $((NEW_FILE_COUNT - 1))" \
	"$(cat "${info[mount0]}/$(printf 'rehash_%05d' $((NEW_FILE_COUNT - 1)))")"

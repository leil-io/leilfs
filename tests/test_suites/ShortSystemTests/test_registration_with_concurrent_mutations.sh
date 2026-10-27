timeout_set '2 minutes'

# Verify paced pull registration preserves chunks during file creation and
# removal. Check convergence, surviving mutations' locations and data.
# Promote a shadow to reuse scanned disks that can accept concurrent writes.
# Rehashing is covered by test_registration_rehash_during_sweep.

CHUNK_COUNT=${CHUNK_COUNT:-100000}
CHUNKS_PER_FILE=1000
REGISTRATION_CHUNKS_PER_SECOND=10000

# Keep name storage consistent across promotion.
create_bdb_name_storage_entry_() {
	echo 'USE_BDB_FOR_NAME_STORAGE = 0'
}

MASTERSERVERS=2 \
	CHUNKSERVERS=2 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
	MASTER_EXTRA_CONFIG="CHUNK_REGISTRATION_CHUNKS_PER_SECOND = ${REGISTRATION_CHUNKS_PER_SECOND}" \
	CHUNKSERVER_EXTRA_CONFIG="MASTER_RECONNECTION_DELAY = 1" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

syslog_file="${ERROR_DIR}/syslog.log"

# Seed metadata offline with disjoint chunk ID ranges at goal 1.
saunafs_master_daemon stop

metadata_version=$(metadata_get_version "${info[master_data_path]}/metadata.sfs")
generate_mock_chunks_changelog "$((2 * CHUNK_COUNT))" "$CHUNKS_PER_FILE" "$metadata_version" \
	> "${info[master_data_path]}/changelog.sfs"
assert_success sfsmetarestore -a -d "${info[master_data_path]}"

saunafs_master_daemon start
saunafs_wait_for_all_ready_chunkservers

for csid in 0 1; do
	mock_dir="$RAMDISK_DIR/mock_disk_$csid"
	mkdir -p "$mock_dir"
	echo "mock:${CHUNK_COUNT}:$((csid * CHUNK_COUNT + 1)):${mock_dir}" \
		>> "${info[chunkserver${csid}_hdd]}"
	saunafs_chunkserver_daemon "$csid" reload
done

assert_eventually_equals "echo $((2 * CHUNK_COUNT))" 'count_chunk_copies'
echo "PHASE: initial registration complete"

# Synchronize the shadow before promotion.
saunafs_master_n 1 start
assert_eventually "saunafs_shadow_synchronized 1"

# Count only the promoted master's completions. Delayed logs from earlier
# registrations could otherwise be mistaken for completions during mutations.
sweeps_finished_after_promotion() {
	grep -c "master_1\[.*finished master-driven chunk registration" "$syslog_file" | cat
}

# Promote the shadow to start paced re-registration on running chunkservers.
saunafs_master_daemon kill
saunafs_make_conf_for_shadow 0
saunafs_make_conf_for_master 1
saunafs_master_daemon reload

# Wait for partial registration; readiness alone does not prove sweep overlap.
registration_in_progress() {
	local copies

	copies=$(count_chunk_copies)
	[[ -n "$copies" && "$copies" -gt 0 && "$copies" -lt $((2 * CHUNK_COUNT)) ]]
}

assert_eventually 'registration_in_progress'
echo "PHASE: promoted, paced re-registration running" \
	"($(count_chunk_copies) / $((2 * CHUNK_COUNT)) copies known)"

# Create and read back new files during registration.
created=0
for i in $(seq 1 40); do
	echo "mutation data $i" > "${info[mount0]}/mutation_$i"
	created=$((created + 1))
done

for i in $(seq 1 40); do
	MESSAGE="reading back mutation_$i during registration" \
		assert_equals "mutation data $i" "$(cat "${info[mount0]}/mutation_$i")"
done

# Remove one seeded file and ten new files while the sweeps are active.
assert_success rm -f "${info[mount0]}/mock_0000001"

for i in $(seq 31 40); do
	assert_success rm -f "${info[mount0]}/mutation_$i"
	created=$((created - 1))
done

# New files add too few copies to hide the gap to the full seeded inventory.
copies_after_mutations=$(count_chunk_copies)
MESSAGE="mutations must run while the registration sweep is still going" \
	assert_less_than "$copies_after_mutations" "$((2 * CHUNK_COUNT))"

# Require both sweeps to remain active so every insert overlaps registration.
MESSAGE="inserts must land in a registry that is still being swept" \
	assert_equals 0 "$(sweeps_finished_after_promotion)"

echo "PHASE: mutations done at $copies_after_mutations / $((2 * CHUNK_COUNT)) copies," \
	"waiting for convergence"

# Require all surviving seeded and new copies; unlinked copies may linger
# until background deletion completes. One seeded file holds CHUNKS_PER_FILE.
expected_min=$((2 * CHUNK_COUNT - CHUNKS_PER_FILE + created))

copies_at_least() {
	local copies

	copies=$(count_chunk_copies)
	[[ -n "$copies" && "$copies" -ge "$expected_min" ]]
}

assert_eventually 'copies_at_least' '30 seconds'

# Lingering deleted copies can mask missing mutations in the total count.
# Check each survivor with fileinfo, which does not trigger on-demand queries.
surviving_mutations_registered() {
	local i

	for ((i = 1; i <= created; ++i)); do
		[[ "$(count_registered_parts "${info[mount0]}/mutation_$i")" -eq 1 ]] || return 1
	done

	return 0
}

MESSAGE="every surviving mutation must be registered after the sweep" \
	assert_eventually 'surviving_mutations_registered' '30 seconds'

# Verify seeded and surviving new-file contents after convergence.
expected_head="5a5b58595e5f5c5d5253505156575455"
assert_equals "$expected_head" \
	"$(dd if="${info[mount0]}/mock_0000002" bs=16 count=1 2>/dev/null | od -An -tx1 | tr -d ' \n')"

assert_equals "mutation data 1" "$(cat "${info[mount0]}/mutation_1")"

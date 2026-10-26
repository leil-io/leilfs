timeout_set '1 minute'

# Verify pull registration (START, credits, END), reads during the sweep,
# and convergence under the master's global budget.

CHUNK_COUNT=${CHUNK_COUNT:-200000}
CHUNKS_PER_FILE=1000

MASTERSERVERS=1 \
	CHUNKSERVERS=2 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
	MASTER_EXTRA_CONFIG="CHUNK_REGISTRATION_CHUNKS_PER_SECOND = 50000" \
	CHUNKSERVER_EXTRA_CONFIG="MASTER_RECONNECTION_DELAY = 1" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

syslog_file="${ERROR_DIR}/syslog.log"

# Seed goal-1 metadata for disjoint chunkserver inventories.
saunafs_master_daemon stop
metadata_version=$(metadata_get_version "${info[master_data_path]}/metadata.sfs")
generate_mock_chunks_changelog "$((2 * CHUNK_COUNT))" "$CHUNKS_PER_FILE" "$metadata_version" \
	> "${info[master_data_path]}/changelog.sfs"
assert_success sfsmetarestore -a -d "${info[master_data_path]}"

saunafs_master_daemon start
saunafs_wait_for_all_ready_chunkservers

# Load the mock inventories before exercising re-registration.
for csid in 0 1; do
	mock_dir="$RAMDISK_DIR/mock_disk_$csid"
	mkdir -p "$mock_dir"

	echo "mock:${CHUNK_COUNT}:$((csid * CHUNK_COUNT + 1)):${mock_dir}" \
		>> "${info[chunkserver${csid}_hdd]}"
	saunafs_chunkserver_daemon "$csid" reload
done

assert_eventually_equals "echo $((2 * CHUNK_COUNT))" 'count_chunk_copies'
echo "PHASE: initial registration complete"

# Restart both chunkservers to exercise globally paced pull registration.
restart_ts=$(date +%s%N)
for csid in 0 1; do
	saunafs_chunkserver_daemon "$csid" restart
done

# Require partial inventory before probing; capacity alone is insufficient.
registration_in_progress() {
	local copies
	copies=$(count_chunk_copies)
	[[ -n "$copies" && "$copies" -gt 0 && "$copies" -lt $((2 * CHUNK_COUNT)) ]]
}

assert_eventually 'registration_in_progress'
echo "PHASE: chunkservers restarted" \
	"($(count_chunk_copies) / $((2 * CHUNK_COUNT)) copies known)"

# Probe an uncached file while the inventories are incomplete.
total_files=$((2 * CHUNK_COUNT / CHUNKS_PER_FILE))
probe_file="${info[mount0]}/$(printf 'mock_%07d' $((total_files / 2)))"

probe_start_ns=$(date +%s%N)
assert_eventually 'dd if="$probe_file" bs=16 count=1 > /dev/null 2>&1'

probe_ms=$(( ($(date +%s%N) - probe_start_ns) / 1000000 ))
copies_at_probe=$(count_chunk_copies)
echo "PULL_ON_DEMAND_PROBE_MS: $probe_ms (copies known at probe: $copies_at_probe / $((2 * CHUNK_COUNT)))"

assert_less_than "$probe_ms" 5000

# The read must finish before the inventories converge.
assert_less_than "$copies_at_probe" "$((2 * CHUNK_COUNT))"

# Wait for both inventories to converge.
assert_eventually_equals "echo $((2 * CHUNK_COUNT))" 'count_chunk_copies'
registration_ms=$(( ($(date +%s%N) - restart_ts) / 1000000 ))
echo "PULL_FULL_REGISTRATION_MS: $registration_ms (chunks: $((2 * CHUNK_COUNT)), budget: 50000/s)"

# Expect START and END for both chunkservers at startup and after restart.
# Reconnects during setup can add more rounds, so use a lower bound.
pull_started_count() {
	grep -c "master-driven chunk registration started" "$syslog_file" | cat
}

pull_finished_count() {
	grep -c "finished master-driven chunk registration" "$syslog_file" | cat
}

assert_eventually '[[ $(pull_started_count) -ge 4 ]]'
assert_eventually '[[ $(pull_finished_count) -ge 4 ]]'

# At the default scale (400k copies at 50k/s), require over 6s to prove pacing.
assert_less_than 6000 "$registration_ms"

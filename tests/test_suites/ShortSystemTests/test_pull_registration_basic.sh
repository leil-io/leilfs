timeout_set '15 minutes'

# Master-driven (pull) chunk registration: a pull-capable chunkserver waits
# for SAU_MATOCS_REGISTER_CHUNKS_START and streams its chunks paced by the
# master's credits; the stream ends with SAU_CSTOMA_REGISTER_CHUNKS_END.
# Verifies the handshake happens, registration converges while paced by the
# global CHUNK_REGISTRATION_CHUNKS_PER_SECOND budget, and client IO on
# not-yet-registered chunks is still served on demand during the paced window.

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

# Seed metadata offline; disjoint id ranges per chunkserver at goal 1
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

assert_eventually_equals "echo $((2 * CHUNK_COUNT))" 'count_chunk_copies' '5 minutes'
echo "PHASE: initial registration complete"

# Restart both chunkservers: their re-registration must go through the pull
# protocol, paced by the master's global budget
restart_ts=$(date +%s%N)
for csid in 0 1; do
	saunafs_chunkserver_daemon "$csid" restart
done
saunafs_wait_for_all_ready_chunkservers
echo "PHASE: chunkservers restarted"

# On-demand resolution still works during the paced registration window
total_files=$((2 * CHUNK_COUNT / CHUNKS_PER_FILE))
probe_file="${info[mount0]}/$(printf 'mock_%07d' $((total_files / 2)))"
probe_start_ns=$(date +%s%N)
assert_eventually 'dd if="$probe_file" bs=16 count=1 > /dev/null 2>&1' '2 minutes'
probe_ms=$(( ($(date +%s%N) - probe_start_ns) / 1000000 ))
echo "PULL_ON_DEMAND_PROBE_MS: $probe_ms"
assert_less_than "$probe_ms" 5000

# Registration converges under the pull protocol
assert_eventually_equals "echo $((2 * CHUNK_COUNT))" 'count_chunk_copies' '10 minutes'
registration_ms=$(( ($(date +%s%N) - restart_ts) / 1000000 ))
echo "PULL_FULL_REGISTRATION_MS: $registration_ms (chunks: $((2 * CHUNK_COUNT)), budget: 50000/s)"

# The pull handshake actually happened, on both sides (>= 4: two
# chunkservers, at least the initial registration and the restart; extra
# rounds come from reconnects, e.g. after the seeding master restart)
pull_started_count() {
	grep -c "master-driven chunk registration started" "$syslog_file" | cat
}
pull_finished_count() {
	grep -c "finished master-driven chunk registration" "$syslog_file" | cat
}
assert_eventually '[[ $(pull_started_count) -ge 4 ]]' '1 minute'
assert_eventually '[[ $(pull_finished_count) -ge 4 ]]' '1 minute'

# The budget-paced re-registration of 400k copies at 50k/s must take >= ~7s
# (this is the DOS-guard actually pacing the stream)
assert_less_than 6000 "$registration_ms"

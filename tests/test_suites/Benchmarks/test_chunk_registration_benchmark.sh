timeout_set '2 hours'

# Chunk registration benchmark. Not pass/fail -- prints numbers to compare
# across builds.
#
# Measures, for a cluster whose chunkservers host CHUNK_COUNT fake chunks each
# (mock disk plugin, no physical chunk files):
#   1. initial registration:   mock disks attached -> master knows all chunks
#   2. promotion IO recovery:  shadow promoted -> client IO works again
#   3. promotion registration: shadow promoted -> master knows all chunks again
#   4. master/chunkserver RSS
#
# Parameters (env):
#   CHUNK_COUNT    fake chunks per chunkserver (default 1'000'000;
#                  manual runs on big machines: 100-200M, budget ~180B/chunk
#                  of chunkserver RAM plus master-side chunk entries)
#   MOCK_CS_COUNT  number of chunkservers (default 2, also used as goal)

CHUNK_COUNT=${CHUNK_COUNT:-1000000}
MOCK_CS_COUNT=${MOCK_CS_COUNT:-2}

MASTERSERVERS=2 \
	CHUNKSERVERS=$MOCK_CS_COUNT \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
	CHUNKSERVER_EXTRA_CONFIG="MASTER_RECONNECTION_DELAY = 1|HDD_TEST_FREQ = 100000" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

now_ms() { echo $(($(date +%s%N) / 1000000)); }

wait_for_chunk_count() {
	local expected=$1
	while [[ "$(count_chunk_copies)" != "$expected" ]]; do
		sleep 0.5
	done
}

# Seed metadata offline: MOCK_CS_COUNT*CHUNK_COUNT chunks at the default goal
# (1); each chunkserver hosts a disjoint slice of the id space, so every chunk
# has exactly one copy and no replication/deletion churn can start.
seed_start_ms=$(now_ms)
saunafs_master_daemon stop
metadata_version=$(metadata_get_version "${info[master_data_path]}/metadata.sfs")
generate_mock_chunks_changelog "$((MOCK_CS_COUNT * CHUNK_COUNT))" 10000 "$metadata_version" \
	> "${info[master_data_path]}/changelog.sfs"
assert_success sfsmetarestore -a -d "${info[master_data_path]}"
saunafs_master_daemon start
saunafs_wait_for_all_ready_chunkservers
echo "METADATA_SEEDING_MS: $(($(now_ms) - seed_start_ms))"

# 1. Initial registration: attach mock disks, wait until master sees all
initial_start_ms=$(now_ms)
for ((csid = 0; csid < MOCK_CS_COUNT; ++csid)); do
	mock_dir="$RAMDISK_DIR/mock_disk_$csid"
	mkdir -p "$mock_dir"
	echo "mock:${CHUNK_COUNT}:$((csid * CHUNK_COUNT + 1)):${mock_dir}" \
		>> "${info[chunkserver${csid}_hdd]}"
	saunafs_chunkserver_daemon "$csid" reload
done
wait_for_chunk_count "$((MOCK_CS_COUNT * CHUNK_COUNT))"
echo "INITIAL_REGISTRATION_MS: $(($(now_ms) - initial_start_ms))"

# Canary file for the recovery measurement
echo "canary data" > "${info[mount0]}/canary"

# Start and sync the shadow
saunafs_master_n 1 start
assert_eventually "saunafs_shadow_synchronized 1" '10 minutes'

# 2+3. Promote the shadow; measure client-IO recovery and full re-registration
saunafs_master_daemon kill
saunafs_make_conf_for_shadow 0
saunafs_make_conf_for_master 1
promotion_start_ms=$(now_ms)
saunafs_master_daemon reload

# Probe files across the id space, none read before the promotion (the mount
# caches chunk locations, so previously-read files bypass the master)
total_files=$((MOCK_CS_COUNT * CHUNK_COUNT / 10000))
probe_files=()
for fraction_idx in 0 1 2 3 4; do
	file_idx=$((fraction_idx * (total_files - 1) / 4))
	probe_files+=("${info[mount0]}/$(printf 'mock_%07d' "$file_idx")")
done
probes_readable() {
	local file
	for file in "${probe_files[@]}"; do
		dd if="$file" bs=16 count=1 &> /dev/null || return 1
	done
	return 0
}
while ! { cat "${info[mount0]}/canary" &> /dev/null && probes_readable; }; do
	sleep 0.2
done
echo "PROMOTION_CLIENT_IO_RECOVERY_MS: $(($(now_ms) - promotion_start_ms))"

# +1: the canary chunk on the real disk
wait_for_chunk_count "$((MOCK_CS_COUNT * CHUNK_COUNT + 1))"
echo "PROMOTION_FULL_REGISTRATION_MS: $(($(now_ms) - promotion_start_ms))"

# 4. Memory footprint (the promoted master runs with master1's cfg)
# several processes can match (symlink names, transient reload commands);
# the actual master is the one with the largest RSS
master_pid=$(pgrep -f "sfsmaster|leil-master" | xargs -r ps -o pid=,rss= -p 2>/dev/null \
	| sort -k2 -rn | head -1 | awk '{print $1}')
[[ $master_pid ]] && echo "MASTER_RSS_KB: $(awk '/VmRSS/ {print $2}' /proc/$master_pid/status)"
cs_pid=$(pgrep -f "(sfschunkserver|leil-chunkserver).*${info[chunkserver0_cfg]}" | head -1)
[[ $cs_pid ]] && echo "CHUNKSERVER_RSS_KB: $(awk '/VmRSS/ {print $2}' /proc/$cs_pid/status)"

echo "CHUNK_COUNT: $CHUNK_COUNT, MOCK_CS_COUNT: $MOCK_CS_COUNT"

timeout_set '2 minutes'

# After promotion, chunk locations are rebuilt through re-registration.
# On-demand queries must restore client I/O well before inventory completes.
# Probe uncached files across the chunk-ID range to exercise missing locations.

CHUNK_COUNT=${CHUNK_COUNT:-1000000}
CHUNKS_PER_FILE=10000
# Require recovery within one third of registration, with a 5s floor.
MAX_RECOVERY_FLOOR_MS=5000

# Keep name storage consistent when promotion reloads the generated configs.
create_bdb_name_storage_entry_() {
	echo 'USE_BDB_FOR_NAME_STORAGE = 0'
}

MASTERSERVERS=2 \
	CHUNKSERVERS=2 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
	MASTER_EXTRA_CONFIG="CHUNK_REGISTRATION_CHUNKS_PER_SECOND = 100000" \
	CHUNKSERVER_EXTRA_CONFIG="MASTER_RECONNECTION_DELAY = 1" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

# Match goal-1 metadata to disjoint mock inventories, giving each chunk
# exactly one copy.
saunafs_master_daemon stop
metadata_version=$(metadata_get_version "${info[master_data_path]}/metadata.sfs")
generate_mock_chunks_changelog "$((2 * CHUNK_COUNT))" "$CHUNKS_PER_FILE" "$metadata_version" \
	> "${info[master_data_path]}/changelog.sfs"
assert_success sfsmetarestore -a -d "${info[master_data_path]}"
saunafs_master_daemon start
saunafs_wait_for_all_ready_chunkservers

# Add the matching inventory to each chunkserver.
for csid in 0 1; do
	mock_dir="$RAMDISK_DIR/mock_disk_$csid"
	mkdir -p "$mock_dir"
	echo "mock:${CHUNK_COUNT}:$((csid * CHUNK_COUNT + 1)):${mock_dir}" \
		>> "${info[chunkserver${csid}_hdd]}"
	saunafs_chunkserver_daemon "$csid" reload
done

assert_eventually_equals "echo $((2 * CHUNK_COUNT))" 'count_chunk_copies'

# The canary uses a real disk and adds one chunk to the expected total.
echo "canary data" > "${info[mount0]}/canary"
assert_equals "canary data" "$(cat "${info[mount0]}/canary")"

# Spread probes across the ID range because registry traversal order varies.
# Use separate sanity files so their reads cannot cache probe locations.
total_files=$((2 * CHUNK_COUNT / CHUNKS_PER_FILE))
probe_files=()
sanity_files=()
for fraction_idx in 0 1 2 3 4; do
	file_idx=$((fraction_idx * (total_files - 2) / 4))
	probe_files+=("${info[mount0]}/$(printf 'mock_%07d' "$file_idx")")
	sanity_files+=("${info[mount0]}/$(printf 'mock_%07d' "$((file_idx + 1))")")
done
files_readable() {
	local file
	for file in "$@"; do
		dd if="$file" bs=16 count=1 > /dev/null 2>&1 || return 1
	done
	return 0
}
# Check mock reads without warming the probe locations.
assert_success files_readable "${sanity_files[@]}"

# Pacing stretches registration to expose recovery before inventory completion.
# Start the shadow and wait for metadata synchronization.
saunafs_master_n 1 start
assert_eventually "saunafs_shadow_synchronized 1"

# Promote the synchronized shadow.
saunafs_master_daemon kill
saunafs_make_conf_for_shadow 0
saunafs_make_conf_for_master 1
promotion_start_ts=$(date +%s%N)
saunafs_master_daemon reload

# Measure recovery of the canary and every uncached probe.
assert_eventually '
	cat "${info[mount0]}/canary" > /dev/null 2>&1 &&
	files_readable "${probe_files[@]}"
'
recovery_ms=$(( ($(date +%s%N) - promotion_start_ts) / 1000000 ))

echo "PROMOTION_CLIENT_IO_RECOVERY_MS: $recovery_ms (chunks per cs: $CHUNK_COUNT)"

# Measure full registration, including the canary's extra chunk.
assert_eventually_equals "echo $((2 * CHUNK_COUNT + 1))" 'count_chunk_copies' '30 seconds'
full_registration_ms=$(( ($(date +%s%N) - promotion_start_ts) / 1000000 ))
echo "PROMOTION_FULL_REGISTRATION_MS: $full_registration_ms (chunks per cs: $CHUNK_COUNT)"

max_recovery_ms=$((full_registration_ms / 3))
if ((max_recovery_ms < MAX_RECOVERY_FLOOR_MS)); then
	max_recovery_ms=$MAX_RECOVERY_FLOOR_MS
fi
assert_less_than "$recovery_ms" "$max_recovery_ms"

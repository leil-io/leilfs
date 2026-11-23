timeout_set '2 minutes'

# After promotion, an uncached read must resolve its missing chunk location
# on demand before full registration completes.

CHUNK_COUNT=${CHUNK_COUNT:-1000000}
CHUNKS_PER_FILE=10000
MAX_PROBE_SECONDS=3

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

# Seed goal-1 metadata matching disjoint chunkserver inventories.
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

# Pace registration so the probe can run before the inventory completes.
saunafs_master_n 1 start
assert_eventually "saunafs_shadow_synchronized 1"

saunafs_master_daemon kill
saunafs_make_conf_for_shadow 0
saunafs_make_conf_for_master 1
saunafs_master_daemon reload

# Probe an unread file in the middle of the ID range to avoid cached locations.
total_files=$((2 * CHUNK_COUNT / CHUNKS_PER_FILE))
probe_file="${info[mount0]}/$(printf 'mock_%07d' $((total_files / 2)))"
probe_start_ns=$(date +%s%N)
assert_eventually 'dd if="$probe_file" bs=16 count=1 > /dev/null 2>&1'
probe_ms=$(( ($(date +%s%N) - probe_start_ns) / 1000000 ))
copies_at_probe=$(count_chunk_copies)

echo "ON_DEMAND_PROBE_MS: $probe_ms (copies known at probe: $copies_at_probe / $((2 * CHUNK_COUNT)))"

# Require a fast read while registration is still incomplete.
assert_less_than "$copies_at_probe" "$((2 * CHUNK_COUNT))"
assert_less_than "$probe_ms" "$((MAX_PROBE_SECONDS * 1000))"

# Verify that the full inventory eventually registers.
assert_eventually_equals "echo $((2 * CHUNK_COUNT))" 'count_chunk_copies' '40 seconds'

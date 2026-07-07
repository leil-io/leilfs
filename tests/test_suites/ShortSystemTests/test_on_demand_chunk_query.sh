timeout_set '15 minutes'

# Verifies the on-demand chunk-location query (M1): right after a shadow
# promotion, while chunkservers are still re-registering their chunks, a read
# of a chunk whose location is not registered yet must be answered through
# SAU_MATOCS_QUERY_CHUNKS instead of stalling until the full registration
# reaches that chunk.

CHUNK_COUNT=${CHUNK_COUNT:-1000000}
CHUNKS_PER_FILE=10000
MAX_PROBE_SECONDS=3

MASTERSERVERS=2 \
	CHUNKSERVERS=2 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
	CHUNKSERVER_EXTRA_CONFIG="MASTER_RECONNECTION_DELAY = 1" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

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

count_chunk_copies() {
	saunafs-admin info localhost "${info[matocl]}" 2>/dev/null \
		| awk '/^Chunk copies:/ {print $NF}'
}
assert_eventually_equals "echo $((2 * CHUNK_COUNT))" 'count_chunk_copies' '5 minutes'

# Slow down re-registration so the post-promotion registration window is long
# enough to observe the on-demand path racing it
for csid in 0 1; do
	echo "HDD_CHUNK_BULK_SIZE = 1" >> "${info[chunkserver${csid}_cfg]}"
	saunafs_chunkserver_daemon "$csid" reload
done

saunafs_master_n 1 start
assert_eventually "saunafs_shadow_synchronized 1"

saunafs_master_daemon kill
saunafs_make_conf_for_shadow 0
saunafs_make_conf_for_master 1
saunafs_master_daemon reload

# Probe: a file never read before (no location cached in the mount), from the
# middle of the id space. Must be readable quickly via the on-demand query.
total_files=$((2 * CHUNK_COUNT / CHUNKS_PER_FILE))
probe_file="${info[mount0]}/$(printf 'mock_%07d' $((total_files / 2)))"
probe_start_ns=$(date +%s%N)
assert_eventually 'dd if="$probe_file" bs=16 count=1 > /dev/null 2>&1' '2 minutes'
probe_ms=$(( ($(date +%s%N) - probe_start_ns) / 1000000 ))
copies_at_probe=$(count_chunk_copies)

echo "ON_DEMAND_PROBE_MS: $probe_ms (copies known at probe: $copies_at_probe / $((2 * CHUNK_COUNT)))"

# The probe must have been answered while the full registration was still in
# progress -- otherwise this test measured nothing
assert_less_than "$copies_at_probe" "$((2 * CHUNK_COUNT))"
assert_less_than "$probe_ms" "$((MAX_PROBE_SECONDS * 1000))"

# Cluster still converges to the full picture
assert_eventually_equals "echo $((2 * CHUNK_COUNT))" 'count_chunk_copies' '10 minutes'

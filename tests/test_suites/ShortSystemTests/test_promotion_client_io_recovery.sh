timeout_set '30 minutes'

# Regression test for the shadow-promotion chunk-registration bottleneck.
#
# A freshly promoted master rebuilds the chunk->chunkserver map from full
# chunkserver re-registration. Until a chunk's copy is re-registered, reads
# of it get an empty location list and the client can only retry. (The
# 'starting' session gate in matoclserv turns out to be ineffective: right
# after promotion the missing-chunks stats are not computed yet, so sessions
# are accepted almost immediately -- the real stall is per-chunk.)
#
# This test measures the time from promotion until client IO works for files
# spread across the whole chunk-id space and asserts it is much smaller than
# the full re-registration time. It passes since the on-demand chunk-location
# resolution landed: client operations on unregistered chunks are held back
# and resolved through SAU_MATOCS_QUERY_CHUNKS instead of stalling.

CHUNK_COUNT=${CHUNK_COUNT:-1000000}
CHUNKS_PER_FILE=10000
# Client IO must recover much faster than the full re-registration: without
# on-demand resolution (M1) reads stall until their chunk happens to be
# re-registered, so recovery time ~= full registration time. The bound is
# relative with an absolute floor so it holds across machine speeds.
MAX_RECOVERY_FLOOR_MS=5000

MASTERSERVERS=2 \
	CHUNKSERVERS=2 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
	MASTER_EXTRA_CONFIG="CHUNK_REGISTRATION_CHUNKS_PER_SECOND = 100000" \
	CHUNKSERVER_EXTRA_CONFIG="MASTER_RECONNECTION_DELAY = 1" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

# Seed metadata offline: 2*CHUNK_COUNT chunks at the default goal (1); each
# chunkserver hosts a disjoint half of the id space, so every chunk has
# exactly one copy and no replication/deletion churn can start.
saunafs_master_daemon stop
metadata_version=$(metadata_get_version "${info[master_data_path]}/metadata.sfs")
generate_mock_chunks_changelog "$((2 * CHUNK_COUNT))" "$CHUNKS_PER_FILE" "$metadata_version" \
	> "${info[master_data_path]}/changelog.sfs"
assert_success sfsmetarestore -a -d "${info[master_data_path]}"
saunafs_master_daemon start
saunafs_wait_for_all_ready_chunkservers

# Attach a mock disk with CHUNK_COUNT chunks to each chunkserver
for csid in 0 1; do
	mock_dir="$RAMDISK_DIR/mock_disk_$csid"
	mkdir -p "$mock_dir"
	echo "mock:${CHUNK_COUNT}:$((csid * CHUNK_COUNT + 1)):${mock_dir}" \
		>> "${info[chunkserver${csid}_hdd]}"
	saunafs_chunkserver_daemon "$csid" reload
done

assert_eventually_equals "echo $((2 * CHUNK_COUNT))" 'count_chunk_copies' '5 minutes'

# Canary file written through the mount (lands on the real disks).
# From here on the expected copies count includes the canary chunk.
echo "canary data" > "${info[mount0]}/canary"
assert_equals "canary data" "$(cat "${info[mount0]}/canary")"

# Probe files spread across the whole chunk-id space: chunk registration order
# is effectively arbitrary (hash-map iteration), so at least some probes get
# their chunks re-registered late. Client IO is only considered recovered when
# ALL of them are readable.
# IMPORTANT: the mount caches chunk locations, so files read before the
# promotion are readable afterwards without asking the master. The sanity
# check below must therefore use DIFFERENT files than the measurement.
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
# Sanity: mock-backed files readable before the promotion (uses files disjoint
# from the probe set, so no location gets cached for the measurement)
assert_success files_readable "${sanity_files[@]}"

# Re-registration is paced by the masters' CHUNK_REGISTRATION_CHUNKS_PER_SECOND
# budget (set above), emulating the registration duration of a much larger
# cluster at CI-sized chunk counts.
# Start the shadow and wait for full metadata sync
saunafs_master_n 1 start
assert_eventually "saunafs_shadow_synchronized 1"

# Kill the master and promote the shadow
saunafs_master_daemon kill
saunafs_make_conf_for_shadow 0
saunafs_make_conf_for_master 1
promotion_start_ts=$(date +%s%N)
saunafs_master_daemon reload

# Measure time until client IO works again across the whole id space
# (canary + probe files whose locations were never cached by the mount)
assert_eventually '
	cat "${info[mount0]}/canary" > /dev/null 2>&1 &&
	files_readable "${probe_files[@]}"
' '20 minutes'
recovery_ms=$(( ($(date +%s%N) - promotion_start_ts) / 1000000 ))

echo "PROMOTION_CLIENT_IO_RECOVERY_MS: $recovery_ms (chunks per cs: $CHUNK_COUNT)"

# Also report how long the full re-registration takes, for comparison
# (+1: the canary chunk on the real disk)
assert_eventually_equals "echo $((2 * CHUNK_COUNT + 1))" 'count_chunk_copies' '20 minutes'
full_registration_ms=$(( ($(date +%s%N) - promotion_start_ts) / 1000000 ))
echo "PROMOTION_FULL_REGISTRATION_MS: $full_registration_ms (chunks per cs: $CHUNK_COUNT)"

max_recovery_ms=$((full_registration_ms / 3))
if ((max_recovery_ms < MAX_RECOVERY_FLOOR_MS)); then
	max_recovery_ms=$MAX_RECOVERY_FLOOR_MS
fi
assert_less_than "$recovery_ms" "$max_recovery_ms"

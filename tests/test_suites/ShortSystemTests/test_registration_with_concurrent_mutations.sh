timeout_set '15 minutes'

# The resumable pull-registration sweep must not lose chunks when the
# registry mutates mid-sweep: new chunks are created (registry inserts can
# rehash the map under the sweep's bucket cursor) and files are unlinked
# while a budget-paced registration is running.

CHUNK_COUNT=${CHUNK_COUNT:-100000}
CHUNKS_PER_FILE=1000

MASTERSERVERS=1 \
	CHUNKSERVERS=2 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
	MASTER_EXTRA_CONFIG="CHUNK_REGISTRATION_CHUNKS_PER_SECOND = 20000" \
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

assert_eventually_equals "echo $((2 * CHUNK_COUNT))" 'count_chunk_copies' '5 minutes'
echo "PHASE: initial registration complete"

# Restart both chunkservers; the paced re-registration window (200k copies at
# 20k/s ~= 10s) stays open while the mutations below run
for csid in 0 1; do
	saunafs_chunkserver_daemon "$csid" restart
done
saunafs_wait_for_all_ready_chunkservers
echo "PHASE: chunkservers restarted, paced registration running"

# Mutations during the sweep: create many new files (their chunks land on
# the real disks and grow the chunkserver registry mid-sweep), read them
# back, and unlink a seeded mock file plus some of the new files
created=0
for i in $(seq 1 40); do
	echo "mutation data $i" > "${info[mount0]}/mutation_$i"
	created=$((created + 1))
done
for i in $(seq 1 40); do
	MESSAGE="reading back mutation_$i during registration" \
		assert_equals "mutation data $i" "$(cat "${info[mount0]}/mutation_$i")"
done
assert_success rm -f "${info[mount0]}/mock_0000001"
for i in $(seq 31 40); do
	assert_success rm -f "${info[mount0]}/mutation_$i"
	created=$((created - 1))
done
echo "PHASE: mutations done, waiting for convergence"

# Convergence: every remaining mock copy plus every remaining new chunk is
# known (>=: copies of the unlinked chunks disappear later via the regular
# deletion machinery)
expected_min=$((2 * CHUNK_COUNT - CHUNKS_PER_FILE + created))
copies_at_least() {
	local copies
	copies=$(count_chunk_copies)
	[[ -n "$copies" && "$copies" -ge "$expected_min" ]]
}
assert_eventually 'copies_at_least' '10 minutes'

# Data still consistent afterwards
expected_head="5a5b58595e5f5c5d5253505156575455"
assert_equals "$expected_head" \
	"$(dd if="${info[mount0]}/mock_0000002" bs=16 count=1 2>/dev/null | od -An -tx1 | tr -d ' \n')"
assert_equals "mutation data 1" "$(cat "${info[mount0]}/mutation_1")"

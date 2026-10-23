timeout_set '2 minutes'

# Verify pull credits respect the bulk limit when EC parts share a hash bucket.
# The registry hashes only chunk ids, so each chunk's three EC parts collide.
# Rehome two EC(2,1) chunks onto one chunkserver: six records in two buckets.
# With bulk size 1 and budget 1/s, each credit must register only one part.
# Check master-visible progress and timing to cover both sweep and wire pacing.

FILE_COUNT=2
FILE_SIZE=262144
REGISTRATION_CHUNKS_PER_SECOND=1
EXPECTED_PARTS=$((FILE_COUNT * 3))

MASTERSERVERS=1 \
	CHUNKSERVERS=3 \
	DISK_PER_CHUNKSERVER=1 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
	MASTER_CUSTOM_GOALS="10 ec21: \$ec(2,1)" \
	MASTER_EXTRA_CONFIG="CHUNK_REGISTRATION_CHUNKS_PER_SECOND = ${REGISTRATION_CHUNKS_PER_SECOND}`
			`|CHUNK_REGISTRATION_BULK_SIZE = 1`
			`|CHUNKS_LOOP_MIN_TIME = 7200" \
	CHUNKSERVER_EXTRA_CONFIG="MASTER_RECONNECTION_DELAY = 1" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

file_path() {
	echo "${info[mount0]}/ec21/$(printf 'file_%05d' "$1")"
}

ec_dir="${info[mount0]}/ec21"
mkdir "$ec_dir"
assert_success saunafs setgoal ec21 "$ec_dir"
for ((i = 0; i < FILE_COUNT; ++i)); do
	FILE_SIZE=$FILE_SIZE file-generate "$(file_path "$i")"
done

for ((i = 0; i < FILE_COUNT; ++i)); do
	MESSAGE="EC(2,1) file $i must start with all three parts" \
		assert_eventually_equals "echo 3" "count_registered_parts '$(file_path "$i")'"
done
echo "PHASE: $FILE_COUNT EC(2,1) chunks seeded across three chunkservers"

# Stop the other chunkservers to release disk locks before giving their disks
# to chunkserver 0. Keeping all EC parts on one server is a test-only setup.
for csid in 1 2; do
	saunafs_chunkserver_daemon "$csid" stop
done
for ((i = 0; i < FILE_COUNT; ++i)); do
	MESSAGE="after stopping chunkservers 1 and 2, file $i must have one known part" \
		assert_eventually_equals "echo 1" "count_registered_parts '$(file_path "$i")'"
done
for csid in 1 2; do
	cat "${info[chunkserver${csid}_hdd]}" >> "${info[chunkserver0_hdd]}"
done

# Restart to scan all three disks. A bulk-size-one sweep must expose one part
# first, not jump directly to three by consuming a whole bucket.
restart_ns=$(date +%s%N)
saunafs_chunkserver_daemon 0 restart

one_part_registered() {
	local parts
	for ((i = 0; i < FILE_COUNT; ++i)); do
		parts=$(count_registered_parts "$(file_path "$i")")
		[[ "$parts" == 1 ]] && return 0
	done
	return 1
}
MESSAGE="one credit must register only one EC part from its hash bucket" \
	assert_eventually 'one_part_registered'

for ((i = 0; i < FILE_COUNT; ++i)); do
	MESSAGE="all rehomed EC parts of file $i must eventually register" \
		assert_eventually_equals "echo 3" "count_registered_parts '$(file_path "$i")'"
done
registration_ms=$(( ($(date +%s%N) - restart_ns) / 1000000 ))
echo "PULL_BULK_LIMIT_REGISTRATION_MS: $registration_ms ($EXPECTED_PARTS parts at 1/s)"

# Six records need six budget windows, not two whole-bucket grants.
# Allow for the first grant's position within a second, but require more than
# three seconds overall.
MESSAGE="six EC parts with a one-record credit must be paced across budget windows" \
	assert_less_than 3000 "$registration_ms"

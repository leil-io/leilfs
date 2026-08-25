timeout_set '2 minutes'

# A pull-registration credit reserves exactly CHUNK_REGISTRATION_BULK_SIZE
# records. The registry is keyed by (chunk id, part type), but its hash uses
# the id alone, so all EC parts of one chunk deliberately occupy one bucket.
#
# This test creates two EC(2,1) chunks, then stops the chunkservers holding two
# of the three parts and attaches their disks to the remaining chunkserver.
# Its next pull sweep therefore has six records arranged as two buckets of
# three parts. With a one-record bulk and a one-record-per-second budget, the
# first bucket must be sent as three separate credited packets. The old code
# consumed an entire bucket and put all three records on the wire for one
# credit, defeating the master-side rate limit.
#
# It must be an integration test: hddRegistrationSweepNext's result is the
# packet which the master budgets, and a map-only test would not prove that a
# credit cannot make more records visible to the master.

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

# Give chunkserver 0 every part of each chunk. The other servers must be
# stopped first, otherwise their disk locks prevent chunkserver 0 from loading
# those disks. This is test-only disk reassignment; production never puts all
# EC parts of one chunk on one server.
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

# Restarting chunkserver 0 now scans all three disks. Its map has three part
# records in each affected bucket. A correct bulk-size-one sweep exposes one
# part first; the buggy whole-bucket sweep jumps directly to three.
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

# Six part records need six budget windows. A restart and the first grant can
# overlap a second boundary, but a correct stream still needs more than three
# seconds. The buggy code sends three records per credit and finishes in two
# windows.
MESSAGE="six EC parts with a one-record credit must be paced across budget windows" \
	assert_less_than 3000 "$registration_ms"

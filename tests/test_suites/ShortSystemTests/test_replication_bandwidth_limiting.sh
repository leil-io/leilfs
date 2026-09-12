timeout_set "90 seconds"

replication_limit=1024
CHUNKSERVERS=2 \
	USE_RAMDISK=YES \
	CHUNKSERVER_EXTRA_CONFIG="REPLICATION_BANDWIDTH_LIMIT_KBPS=$replication_limit" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	MASTER_EXTRA_CONFIG="CHUNKS_LOOP_MIN_TIME = 1`
			`|CHUNKS_LOOP_MAX_CPU = 90`
			`|CHUNKS_WRITE_REP_LIMIT = 1000`
			`|CHUNKS_READ_REP_LIMIT = 100`
			`|OPERATIONS_DELAY_INIT = 0`
			`|OPERATIONS_DELAY_DISCONNECT = 0" \
	setup_local_empty_saunafs info

# Chunks the emptied chunkserver has taken a complete copy of. A replication in flight holds its
# files at version zero until it commits, so counting those would end the wait when the last
# transfer starts rather than when it finishes.
fully_replicated_chunks_on_emptied_chunkserver() {
	find_chunkserver_metadata_chunks 0 \
		-not -name "*_00000000${chunk_metadata_extension}" | wc -l
}

# Without the measurement row: this compares the counts, and a backend that measures chunk health
# in the background reports when it did, which moves on every measurement.
chunks_health() {
	saunafs_admin_command chunks-health --porcelain localhost "${info[matocl]}" | grep -v "^MEA "
}

cd "${info[mount0]}"
mkdir dir
saunafs setgoal 2 dir
cd dir

file_size_kb=$((5 * 1024)) # test assumes that this is less or equal to chunk size
chunks_count=11
FILE_SIZE=${file_size_kb}K file-generate $(seq 1 $chunks_count)

assert_equals $chunks_count $(find_chunkserver_metadata_chunks 0 | wc -l)

# The healthy report to come back to has to be one that covers the chunks just written, not
# whatever the last measurement of a backend that takes them in the background happened to hold.
wait_for_chunk_health_measurement "${info[matocl]}"
health_ok=$(chunks_health)
saunafs_chunkserver_daemon 0 stop

find_chunkserver_metadata_chunks 0 | xargs -d'\n' rm -f
assert_equals 0 $(find_chunkserver_metadata_chunks 0 | wc -l)
assert_equals $chunks_count $(find_chunkserver_metadata_chunks 1 | wc -l)

saunafs_chunkserver_daemon 0 start

start_TS=$(timestamp)
expected_time_s=$((file_size_kb * chunks_count / replication_limit))
accepted_inaccuracy_s=5
if valgrind_enabled; then
	accepted_inaccuracy_s=30
fi;
# Timed on the copies landing on the emptied chunkserver's disk, which is what the limit
# governs; the in-memory master's report has to agree in the same window, as before. A measuring
# backend's report is not the clock: its records still name this server for the deleted files.
replication_complete() {
	[ "$chunks_count" == "$(fully_replicated_chunks_on_emptied_chunkserver)" ] || return 1
	[[ "${METADATA_BACKEND:-}" == "FDB" ]] || [ "$health_ok" == "$(chunks_health)" ]
}
assert_success wait_for replication_complete \
		"$((expected_time_s + accepted_inaccuracy_s)) seconds"
end_TS=$(timestamp)
assert_near $expected_time_s $((end_TS - start_TS)) $accepted_inaccuracy_s

# The report has to say so too. On a measuring backend only a measurement taken after the copies
# landed can, and timing it would measure the report's delay, so it comes after the clock.
wait_for_chunk_health_measurement "${info[matocl]}"
assert_eventually '[ "$health_ok" == "$(chunks_health)" ]'


timeout_set '15 minutes'

# A write to an EXISTING chunk whose copy is not registered yet must be held
# back by the master and answered once the on-demand location query
# (SAU_MATOCS_QUERY_CHUNKS) resolves -- the write-path counterpart of
# test_on_demand_chunk_query.sh.
#
# Before this was handled, the master answered CHUNKLOST straight away for such
# a chunk, and the client could only burn its bounded retry budget
# (sfsioretries) and fail with EIO. Reads of the same chunk were already served
# on demand, so the two paths disagreed about the same cluster state.
#
# Real chunks on a real disk are used deliberately, NOT the mock disk plugin:
# a write to an existing chunk bumps the chunk version, which renames the chunk
# file on disk, and mock chunks have no file to rename.
#
# The registration window is opened by restarting the single chunkserver and
# pacing re-registration at CHUNK_REGISTRATION_CHUNKS_PER_SECOND with
# CHUNK_REGISTRATION_BULK_SIZE = 1 -- the bulk size matters, since the default
# of 1000 would carry every chunk in the first bulk and close the window
# instantly. Note the test must NOT wait for "ready" chunkservers after the
# restart: a chunkserver only counts as ready once registration has COMPLETED,
# which is exactly the state this test needs to avoid.

FILE_COUNT=${FILE_COUNT:-300}
REGISTRATION_CHUNKS_PER_SECOND=2
PROBE_COUNT=5
MAX_PROBE_SECONDS=10

CHUNKSERVERS=1 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
	MASTER_EXTRA_CONFIG="CHUNK_REGISTRATION_CHUNKS_PER_SECOND = ${REGISTRATION_CHUNKS_PER_SECOND}|CHUNK_REGISTRATION_BULK_SIZE = 1" \
	CHUNKSERVER_EXTRA_CONFIG="MASTER_RECONNECTION_DELAY = 1" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER,sfsioretries=3" \
	setup_local_empty_saunafs info

# One small file per chunk, written live: the master already knows every copy,
# so no bulk registration happens until the chunkserver is restarted below.
for ((i = 0; i < FILE_COUNT; ++i)); do
	echo "seed $i" > "${info[mount0]}/$(printf 'file_%05d' "$i")"
done
assert_eventually_equals "echo $FILE_COUNT" 'count_chunk_copies' '3 minutes'
echo "PHASE: $FILE_COUNT chunks created and known to the master"

# Restart the chunkserver: the master drops its copies and the re-registration
# window opens, paced at REGISTRATION_CHUNKS_PER_SECOND chunks per second.
saunafs_chunkserver_daemon 0 restart

# Wait only until the chunkserver has reconnected and registration has STARTED
# (some copies known) while still being far from complete -- deliberately not
# saunafs_wait_for_all_ready_chunkservers, which returns only once registration
# has finished. The chunkserver must be connected for the on-demand query to
# have anyone to ask.
registration_in_progress() {
	local copies
	copies=$(count_chunk_copies)
	[[ -n "$copies" && "$copies" -gt 0 && "$copies" -lt "$FILE_COUNT" ]]
}
assert_eventually 'registration_in_progress' '2 minutes'
echo "PHASE: registration window open ($(count_chunk_copies) / $FILE_COUNT copies known)"

# Probe: in-place writes (conv=notrunc keeps the existing chunk instead of
# truncating the file and allocating a fresh one; conv=fsync makes a deferred
# write error surface in dd itself rather than at close) to the files created
# last, which re-register last and so are still unknown to the master.
probe_write() {
	local idx=$1
	dd if=/dev/zero of="${info[mount0]}/$(printf 'file_%05d' "$idx")" \
		bs=4096 count=1 conv=notrunc,fsync status=none
}

probe_start_ns=$(date +%s%N)
for ((p = 0; p < PROBE_COUNT; ++p)); do
	probe_idx=$((FILE_COUNT - 1 - p))
	MESSAGE="in-place write to not-yet-registered chunk of file_$(printf '%05d' "$probe_idx")" \
		assert_success probe_write "$probe_idx"
done
probe_ms=$(( ($(date +%s%N) - probe_start_ns) / 1000000 ))
copies_at_probe=$(count_chunk_copies)

echo "WRITE_PROBE_MS: $probe_ms for $PROBE_COUNT writes" \
	"(copies known at probe: $copies_at_probe / $FILE_COUNT)"

# The probes must have been served while registration was still in progress --
# otherwise this test measured nothing
assert_less_than "$copies_at_probe" "$FILE_COUNT"
assert_less_than "$probe_ms" "$((MAX_PROBE_SECONDS * 1000))"

# Cluster still converges to the full picture afterwards
assert_eventually_equals "echo $FILE_COUNT" 'count_chunk_copies' '10 minutes'

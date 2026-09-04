timeout_set '3 minutes'

# Read-path counterpart of test_write_during_registration.sh, run against the
# very same scenario: a chunk that exists in metadata while its only copy is
# not registered yet, during a paced chunkserver re-registration window.
#
# A read of such a chunk must be held back by the master and answered once the
# on-demand location query (SAU_MATOCS_QUERY_CHUNKS) resolves, rather than
# returning an empty location list the client can only retry against.
#
# Keeping this test next to the write one is what makes either result readable:
# they drive the same cluster state and differ only in the operation.
#
# The mount is remounted before the probe on purpose: these files are written
# by this test, and a warm mount cache could serve the read without ever asking
# the master, which would make the probe vacuous.

FILE_COUNT=${FILE_COUNT:-300}
REGISTRATION_CHUNKS_PER_SECOND=5
PROBE_COUNT=10
MAX_PROBE_SECONDS=15

CHUNKSERVERS=1 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
	MASTER_EXTRA_CONFIG="CHUNK_REGISTRATION_CHUNKS_PER_SECOND = ${REGISTRATION_CHUNKS_PER_SECOND}|CHUNK_REGISTRATION_BULK_SIZE = 1" \
	CHUNKSERVER_EXTRA_CONFIG="MASTER_RECONNECTION_DELAY = 1" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

# One small file per chunk, written live: the master already knows every copy,
# so no bulk registration happens until the chunkserver is restarted below.
for ((i = 0; i < FILE_COUNT; ++i)); do
	echo "seed $i" > "${info[mount0]}/$(printf 'file_%05d' "$i")"
done
assert_eventually_equals "echo $FILE_COUNT" 'count_chunk_copies'
echo "PHASE: $FILE_COUNT chunks created and known to the master"

# Drop every client-side cache, so the probe reads below must go to the master
# for a chunk location instead of being served locally
saunafs_mount_unmount 0
saunafs_mount_start 0
echo "PHASE: mount restarted, client caches cold"

# Restart the chunkserver: the master drops its copies and the re-registration
# window opens, paced at REGISTRATION_CHUNKS_PER_SECOND chunks per second.
saunafs_chunkserver_daemon 0 restart

# Wait only until the chunkserver has reconnected and registration has STARTED
# (some copies known) while still being far from complete -- deliberately not
# saunafs_wait_for_all_ready_chunkservers, which returns only once registration
# has finished (measured: it blocks for essentially the whole window). The
# chunkserver must be connected for the on-demand query to have anyone to ask.
registration_in_progress() {
	local copies
	copies=$(count_chunk_copies)
	[[ -n "$copies" && "$copies" -gt 0 && "$copies" -lt "$FILE_COUNT" ]]
}
assert_eventually 'registration_in_progress'
echo "PHASE: registration window open ($(count_chunk_copies) / $FILE_COUNT copies known)"

# Pick chunks whose copy the master does not know about yet, by measuring
# rather than by position: a chunkserver streams its chunks in its own internal
# order, not by chunk id, so "created last" says nothing about "registers
# last". Reading a chunk that happens to be registered already would be served
# from a known location and prove nothing about the deferred path.
probe_indices=()
for ((i = FILE_COUNT - 1; i >= 0; --i)); do
	[[ ${#probe_indices[@]} -lt $PROBE_COUNT ]] || break
	if [[ "$(count_registered_parts "${info[mount0]}/$(printf 'file_%05d' "$i")")" -eq 0 ]]; then
		probe_indices+=("$i")
	fi
done
MESSAGE="need $PROBE_COUNT chunks whose copy is not registered yet" \
	assert_equals "$PROBE_COUNT" "${#probe_indices[@]}"

# Probe: content is verified too: a deferred read must return the real data,
# not a short or empty result.
probe_start_ns=$(date +%s%N)
for probe_idx in "${probe_indices[@]}"; do
	probe_file="${info[mount0]}/$(printf 'file_%05d' "$probe_idx")"
	MESSAGE="read of not-yet-registered chunk of file_$(printf '%05d' "$probe_idx")" \
		assert_equals "seed $probe_idx" "$(cat "$probe_file")"
done
probe_ms=$(( ($(date +%s%N) - probe_start_ns) / 1000000 ))
copies_at_probe=$(count_chunk_copies)

echo "READ_PROBE_MS: $probe_ms for $PROBE_COUNT reads" \
	"(copies known at probe: $copies_at_probe / $FILE_COUNT)"

# The probes must have been served while registration was still in progress,
# otherwise this test measured nothing
assert_less_than "$copies_at_probe" "$FILE_COUNT"
assert_less_than "$probe_ms" "$((MAX_PROBE_SECONDS * 1000))"

# Cluster still converges to the full picture afterwards
assert_eventually_equals "echo $FILE_COUNT" 'count_chunk_copies' '2 minutes'

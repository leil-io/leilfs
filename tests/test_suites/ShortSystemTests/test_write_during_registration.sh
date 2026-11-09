timeout_set '4 minutes'

# Writes to existing chunks with unregistered copies must wait for on-demand
# location queries instead of returning CHUNKLOST and exhausting retries.
# Use real disks: version bumps rename chunk files absent from mock disks.
# Pace registration with one-record bulks to keep inventory incomplete.

FILE_COUNT=${FILE_COUNT:-300}
REGISTRATION_CHUNKS_PER_SECOND=5
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

# Write one file per chunk so the master knows every copy before the restart.
for ((i = 0; i < FILE_COUNT; ++i)); do
	echo "seed $i" > "${info[mount0]}/$(printf 'file_%05d' "$i")"
done

assert_eventually_equals "echo $FILE_COUNT" 'count_chunk_copies' '3 minutes'
echo "PHASE: $FILE_COUNT chunks created and known to the master"

# Restart to clear the master's copy inventory and begin paced re-registration.
saunafs_chunkserver_daemon 0 restart

# Require a partially registered inventory, not just advertised capacity.
registration_in_progress() {
	local copies
	copies=$(count_chunk_copies)
	[[ -n "$copies" && "$copies" -gt 0 && "$copies" -lt "$FILE_COUNT" ]]
}

assert_eventually 'registration_in_progress'
echo "PHASE: registration window open ($(count_chunk_copies) / $FILE_COUNT copies known)"

# Select unreported copies explicitly: registration order does not follow IDs.
probe_indices=()
for ((i = FILE_COUNT - 1; i >= 0; --i)); do
	[[ ${#probe_indices[@]} -lt $PROBE_COUNT ]] || break
	if [[ "$(count_registered_parts "${info[mount0]}/$(printf 'file_%05d' "$i")")" -eq 0 ]]; then
		probe_indices+=("$i")
	fi
done

MESSAGE="need $PROBE_COUNT chunks whose copy is not registered yet" \
	assert_equals "$PROBE_COUNT" "${#probe_indices[@]}"

# notrunc preserves the existing chunk; fsync exposes write errors in dd.
probe_write() {
	local idx=$1
	dd if=/dev/zero of="${info[mount0]}/$(printf 'file_%05d' "$idx")" \
		bs=4096 count=1 conv=notrunc,fsync status=none
}

probe_start_ns=$(date +%s%N)
for probe_idx in "${probe_indices[@]}"; do
	MESSAGE="in-place write to not-yet-registered chunk of file_$(printf '%05d' "$probe_idx")" \
		assert_success probe_write "$probe_idx"
done

probe_ms=$(( ($(date +%s%N) - probe_start_ns) / 1000000 ))
copies_at_probe=$(count_chunk_copies)

echo "WRITE_PROBE_MS: $probe_ms for $PROBE_COUNT writes" \
	"(copies known at probe: $copies_at_probe / $FILE_COUNT)"

# Writes must finish before full registration.
assert_less_than "$copies_at_probe" "$FILE_COUNT"
assert_less_than "$probe_ms" "$((MAX_PROBE_SECONDS * 1000))"

# Verify eventual inventory convergence.
assert_eventually_equals "echo $FILE_COUNT" 'count_chunk_copies' '3 minutes'

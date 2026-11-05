timeout_set '3 minutes'

# Read-path counterpart of test_write_during_registration.sh.
# Reads of known chunks with unregistered copies must wait for on-demand
# location queries, then return the expected data during paced registration.

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

# Write one file per chunk so the master knows every copy before the restart.
for ((i = 0; i < FILE_COUNT; ++i)); do
	echo "seed $i" > "${info[mount0]}/$(printf 'file_%05d' "$i")"
done

assert_eventually_equals "echo $FILE_COUNT" 'count_chunk_copies'
echo "PHASE: $FILE_COUNT chunks created and known to the master"

# Remount to clear client caches and force reads to request chunk locations.
saunafs_mount_unmount 0
saunafs_mount_start 0
echo "PHASE: mount restarted, client caches cold"

# Restart to clear the master's copy inventory and begin paced re-registration.
saunafs_chunkserver_daemon 0 restart

# Require partial inventory; available capacity alone does not prove progress.
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

# Verify deferred reads return complete data within the time limit.
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

# Reads must finish before full registration.
assert_less_than "$copies_at_probe" "$FILE_COUNT"
assert_less_than "$probe_ms" "$((MAX_PROBE_SECONDS * 1000))"

# Verify eventual inventory convergence.
assert_eventually_equals "echo $FILE_COUNT" 'count_chunk_copies' '2 minutes'

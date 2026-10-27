timeout_set '2 minutes'

# Verify reads, existing-chunk writes, and new EC(2,1) allocations during promotion.
# Chunkservers reconnect with scanned disks and advertise space before inventory.
# One-record bulks keep registration incomplete while client I/O is checked.

FILE_COUNT=${FILE_COUNT:-100}
FILE_SIZE=${FILE_SIZE:-262144}
REGISTRATION_CHUNKS_PER_SECOND=5
PROBE_COUNT=10
MAX_PROBE_SECONDS=15

# Keep name storage consistent when promotion reloads the generated configs.
create_bdb_name_storage_entry_() {
	echo 'USE_BDB_FOR_NAME_STORAGE = 0'
}

MASTERSERVERS=2 \
	CHUNKSERVERS=3 \
	DISK_PER_CHUNKSERVER=1 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
	MASTER_CUSTOM_GOALS="10 ec21: \$ec(2,1)" \
	MASTER_EXTRA_CONFIG="CHUNK_REGISTRATION_CHUNKS_PER_SECOND = ${REGISTRATION_CHUNKS_PER_SECOND}|CHUNK_REGISTRATION_BULK_SIZE = 1" \
	CHUNKSERVER_EXTRA_CONFIG="MASTER_RECONNECTION_DELAY = 1" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

syslog_file="${ERROR_DIR}/syslog.log"
ec_dir="${info[mount0]}/ec21"

file_path() {
	echo "$ec_dir/$(printf 'file_%05d' "$1")"
}

mkdir "$ec_dir"
assert_success saunafs setgoal ec21 "$ec_dir"

# Seed one real chunk per file, with three EC parts across the servers.
for ((i = 0; i < FILE_COUNT; ++i)); do
	FILE_SIZE=$FILE_SIZE file-generate "$(file_path "$i")"
done

MESSAGE="every chunk must start with all 3 parts known" \
	assert_eventually_equals "echo 3" \
		"count_registered_parts '$(file_path $((FILE_COUNT - 1)))'"

# "Chunk copies" counts full copies, so a complete ec(2,1) chunk contributes 1
assert_eventually_equals "echo $FILE_COUNT" 'count_chunk_copies'
echo "PHASE: $FILE_COUNT real ec(2,1) chunks created, 3 parts each"

# Drop client-side caches, so the reads below must ask the leader for locations
saunafs_mount_unmount 0
saunafs_mount_start 0

# Synchronize the shadow before promotion.
saunafs_master_n 1 start
assert_eventually "saunafs_shadow_synchronized 1"

# Promote the shadow while chunkservers stay running and reconnect.
saunafs_master_daemon kill
saunafs_make_conf_for_shadow 0
saunafs_make_conf_for_master 1
saunafs_master_daemon reload

# Ignore placement errors from setup; only new failures during the probes count.
placement_errors_before=$(grep -c 'No chunk servers' "$syslog_file" | cat)

# Require partial inventory explicitly; capacity readiness alone is insufficient.
registration_in_progress() {
	local copies
	copies=$(count_chunk_copies)
	[[ -n "$copies" && "$copies" -gt 0 && "$copies" -lt "$FILE_COUNT" ]]
}
assert_eventually 'registration_in_progress'
echo "PHASE: promoted, re-registration window open" \
	"($(count_chunk_copies) / $FILE_COUNT full copies known)"

# Select files with fewer than two known parts; registration order is not by ID.
# Keep read/write targets separate so writes cannot alter data being validated.
read_targets=()
write_targets=()
for ((i = FILE_COUNT - 1; i >= 0; --i)); do
	[[ ${#read_targets[@]} -lt $PROBE_COUNT || ${#write_targets[@]} -lt $PROBE_COUNT ]] || break
	candidate=$(file_path "$i")
	[[ "$(count_registered_parts "$candidate")" -lt 2 ]] || continue
	if [[ ${#read_targets[@]} -lt $PROBE_COUNT ]]; then
		read_targets+=("$candidate")
	else
		write_targets+=("$candidate")
	fi
done
MESSAGE="need $PROBE_COUNT read targets still below the ec(2,1) threshold" \
	assert_equals "$PROBE_COUNT" "${#read_targets[@]}"
MESSAGE="need $PROBE_COUNT write targets still below the ec(2,1) threshold" \
	assert_equals "$PROBE_COUNT" "${#write_targets[@]}"

probe_start_ns=$(date +%s%N)

# Resolve missing read locations on demand.
for probe_file in "${read_targets[@]}"; do
	MESSAGE="reading $(basename "$probe_file") while its parts re-register" \
		assert_success file-validate "$probe_file"
done

# Overwrite existing chunks whose known EC parts are still insufficient.
for probe_file in "${write_targets[@]}"; do
	MESSAGE="writing to $(basename "$probe_file") while its parts re-register" \
		assert_success dd if=/dev/zero of="$probe_file" bs=4096 count=1 \
			conv=notrunc,fsync status=none
done

# Place new EC parts on all three chunkservers before inventory completes.
for ((p = 1; p <= PROBE_COUNT; ++p)); do
	probe_file="$ec_dir/promotion_$p"
	MESSAGE="creating a file while chunkservers re-register with the new leader" \
		assert_success bash -c "FILE_SIZE=$FILE_SIZE file-generate '$probe_file'"
	MESSAGE="reading back the file created during re-registration" \
		assert_success file-validate "$probe_file"
	MESSAGE="a file created during re-registration must get all 3 ec parts" \
		assert_equals 3 "$(count_registered_parts "$probe_file")"
done

probe_ms=$(( ($(date +%s%N) - probe_start_ns) / 1000000 ))
copies_at_probe=$(count_chunk_copies)

echo "PROMOTION_IO_PROBE_MS: $probe_ms for $PROBE_COUNT reads," \
	"$PROBE_COUNT writes and $PROBE_COUNT new files" \
	"(full copies known at probe: $copies_at_probe / $FILE_COUNT)"

# Even with new files counted, stay below the seeded total to prove
# the original inventory is still incomplete.
assert_less_than "$copies_at_probe" "$FILE_COUNT"

# Bound latency and require no new placement failures, so retries cannot
# hide unavailable capacity during registration.
assert_less_than "$probe_ms" "$((MAX_PROBE_SECONDS * 1000))"
placement_errors_after=$(grep -c 'No chunk servers' "$syslog_file" | cat)
MESSAGE="client IO must not fall back to the retry loop during a failover" \
	assert_equals "$placement_errors_before" "$placement_errors_after"

# Writes bump versions; late old-version parts may need background repair
# before copy counts converge.
assert_eventually_equals "echo $((FILE_COUNT + PROBE_COUNT))" 'count_chunk_copies' '1 minute'

# Validate original data and new files, then check the overwritten blocks.
for probe_file in "${read_targets[@]}"; do
	assert_success file-validate "$probe_file"
done
for ((p = 1; p <= PROBE_COUNT; ++p)); do
	assert_success file-validate "$ec_dir/promotion_$p"
done
for probe_file in "${write_targets[@]}"; do
	MESSAGE="the recovered chunk must read back what was written during failover" \
		assert_success cmp -n 4096 "$probe_file" /dev/zero
done

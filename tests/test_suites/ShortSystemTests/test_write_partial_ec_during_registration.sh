timeout_set '3 minutes'

# Write-path counterpart of test_read_partial_ec_during_registration.sh.
# With only one known EC(2,1) part, writes must wait for on-demand queries
# instead of returning CHUNKLOST and exhausting retries.
# Two known parts already permit degraded writes with default redundancy;
# this test targets the insufficient one-part case.

FILE_COUNT=${FILE_COUNT:-100}
FILE_SIZE=${FILE_SIZE:-262144}
REGISTRATION_CHUNKS_PER_SECOND=2
PROBE_COUNT=5
MAX_PROBE_SECONDS=10

master_extra_config="CHUNK_REGISTRATION_CHUNKS_PER_SECOND = ${REGISTRATION_CHUNKS_PER_SECOND}"
master_extra_config+='|CHUNK_REGISTRATION_BULK_SIZE = 1'
master_extra_config+='|CHUNKS_LOOP_TIME = 1'
master_extra_config+='|OPERATIONS_DELAY_INIT = 0'

CHUNKSERVERS=3 \
	DISK_PER_CHUNKSERVER=1 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
	MASTER_CUSTOM_GOALS="10 ec21: \$ec(2,1)" \
	MASTER_EXTRA_CONFIG="${master_extra_config}" \
	CHUNKSERVER_EXTRA_CONFIG="MASTER_RECONNECTION_DELAY = 1" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER,sfsioretries=3" \
	setup_local_empty_saunafs info

file_path() {
	echo "${info[mount0]}/ec21/$(printf 'file_%05d' "$1")"
}

ec_dir="${info[mount0]}/ec21"
mkdir "$ec_dir"
assert_success saunafs setgoal ec21 "$ec_dir"

# Store one chunk per file, with three EC parts across three servers.
for ((i = 0; i < FILE_COUNT; ++i)); do
	FILE_SIZE=$FILE_SIZE file-generate "$(file_path "$i")"
done

first_file=$(file_path 0)
last_file=$(file_path $((FILE_COUNT - 1)))
MESSAGE="every EC(2,1) chunk must start with all 3 parts known" \
	assert_eventually_equals "echo 3" "count_registered_parts '$last_file'"
echo "PHASE: $FILE_COUNT EC(2,1) chunks created, 3 parts each"

# Remount to clear client caches before probing.
saunafs_mount_unmount 0
saunafs_mount_start 0
echo "PHASE: mount restarted, client caches cold"

# Keep server 0 serving one part while the other two re-register.
for csid in 1 2; do
	saunafs_chunkserver_daemon "$csid" restart
done

# A second part proves a restarted server is sending inventory and can be
# queried. Capacity readiness alone does not establish this.
restarted_chunkservers_registering() {
	[[ "$(count_registered_parts "$first_file")" -ge 2 ]]
}

assert_eventually 'restarted_chunkservers_registering'
echo "PHASE: registration window open (first file back to $(count_registered_parts "$first_file") parts)"

# Select files still at one known part; registration order does not follow IDs.
probe_files=()
for ((i = FILE_COUNT - 1; i >= 0; --i)); do
	[[ ${#probe_files[@]} -lt $PROBE_COUNT ]] || break
	candidate=$(file_path "$i")
	if [[ "$(count_registered_parts "$candidate")" -eq 1 ]]; then
		probe_files+=("$candidate")
	fi
done

MESSAGE="need $PROBE_COUNT chunks still below the EC(2,1) threshold; registration outran the probe" \
	assert_equals "$PROBE_COUNT" "${#probe_files[@]}"

# notrunc preserves the existing chunk; fsync exposes write errors in dd.
probe_write() {
	dd if=/dev/zero of="$1" bs=4096 count=1 conv=notrunc,fsync status=none
}

probe_start_ns=$(date +%s%N)
for probe_file in "${probe_files[@]}"; do
	MESSAGE="in-place write to EC(2,1) chunk with only 1 of 3 parts registered, $(basename "$probe_file")" \
		assert_success probe_write "$probe_file"
done

probe_ms=$(( ($(date +%s%N) - probe_start_ns) / 1000000 ))

echo "EC_WRITE_PROBE_MS: $probe_ms for $PROBE_COUNT writes"
assert_less_than "$probe_ms" "$((MAX_PROBE_SECONDS * 1000))"

# Wait for both inventories; available capacity does not imply completion.
for csid in 1 2; do
	wait_for_chunkserver_pull_registration "$csid"
done

# Untouched chunks need only re-registration and must already have all parts.
incomplete=0
for ((i = 0; i < FILE_COUNT; ++i)); do
	candidate=$(file_path "$i")
	probed=0
	for probe_file in "${probe_files[@]}"; do
		[[ "$candidate" == "$probe_file" ]] && probed=1 && break
	done
	[[ $probed -eq 1 ]] && continue
	[[ "$(count_registered_parts "$candidate")" -eq 3 ]] || incomplete=$((incomplete + 1))
done

MESSAGE="every untouched chunk must have all 3 parts registered once the window closes" \
	assert_equals 0 "$incomplete"

# Writes bump versions, so late old-version parts may need background repair.
# Allow queried chunks to regain full redundancy after registration.
probed_chunks_rebuilt() {
	local file
	for file in "${probe_files[@]}"; do
		[[ "$(count_registered_parts "$file")" -eq 3 ]] || return 1
	done
	return 0
}

assert_eventually 'probed_chunks_rebuilt' '1 minute'

# Unreported existing parts must not trigger duplicate background repair.
MESSAGE="registration must not provoke duplicate background replication" \
	assert_failure grep -q 'replication error: failed to create chunk (Chunk already exists)' \
		"$ERROR_DIR/syslog.log"

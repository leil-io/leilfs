timeout_set '2 minutes'

# EC(2,1) needs two parts to read; one known location is insufficient.
# Verify reads wait for on-demand queries during partial registration.
# Count registered parts per file: full-copy counts hide partial EC state,
# and space reports can advertise chunk counts before inventory arrives.

FILE_COUNT=${FILE_COUNT:-100}
FILE_SIZE=${FILE_SIZE:-262144}
REGISTRATION_CHUNKS_PER_SECOND=5
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

# Remount to clear client caches and force reads to request chunk locations.
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

MESSAGE="need $PROBE_COUNT chunks still below the EC(2,1) read threshold; registration outran the probe" \
	assert_equals "$PROBE_COUNT" "${#probe_files[@]}"

# Validate full contents. Capped sfsioretries exposes unusable locations as EIO
# instead of hiding the failure behind retries; also bound read latency.
probe_start_ns=$(date +%s%N)
for probe_file in "${probe_files[@]}"; do
	MESSAGE="read of EC(2,1) chunk with only 1 of 3 parts registered, $(basename "$probe_file")" \
		assert_success file-validate "$probe_file"
done

probe_ms=$(( ($(date +%s%N) - probe_start_ns) / 1000000 ))

echo "EC_READ_PROBE_MS: $probe_ms for $PROBE_COUNT reads"
assert_less_than "$probe_ms" "$((MAX_PROBE_SECONDS * 1000))"

# Wait for both inventories and check every file, beyond the queried targets.
for csid in 1 2; do
	wait_for_chunkserver_pull_registration "$csid"
done

incomplete=0
for ((i = 0; i < FILE_COUNT; ++i)); do
	[[ "$(count_registered_parts "$(file_path "$i")")" -eq 3 ]] || incomplete=$((incomplete + 1))
done

MESSAGE="every chunk must have all 3 parts registered once the window closes" \
	assert_equals 0 "$incomplete"

# Unreported existing parts must not trigger duplicate background repair.
MESSAGE="registration must not provoke duplicate background replication" \
	assert_failure grep -q 'replication error: failed to create chunk (Chunk already exists)' \
		"$ERROR_DIR/syslog.log"

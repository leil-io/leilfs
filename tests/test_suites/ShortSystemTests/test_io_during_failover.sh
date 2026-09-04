timeout_set '2 minutes'

# Production client IO reads, writes to existing files, and creating new
# files must keep working while chunkservers re-register with a freshly
# promoted leader. This is the failover shape, and it differs from every other
# test in this family in a way that decides the outcome: the chunkservers are
# not restarted, they only reconnect.
#
# A chunkserver reports its free space once, in the registration tail it sends
# as soon as it registers, and does not refresh that report until its chunk
# registration has COMPLETED. Which of those two facts dominates depends
# entirely on whether the chunkserver had already scanned its disks:
#
#   restarted   - the tail goes out while the scan is still running, so it
#                 carries no space, nothing refreshes it for the rest of the
#                 window, and the leader can place no new chunk anywhere. New
#                 files can only fail and be retried until registration ends.
#
#   reconnected - the disks were scanned back when the process started, so the
#                 tail carries real space and the leader can place new chunks
#                 immediately, long before the chunk stream has caught up.
#
# This test exercises the second scenario, which is what a failover looks like.
# The other tests in this family restart chunkservers, which is the harsher
# variant and useful for the read paths, but it cannot show this behaviour at all.
#
# All three operations are probed together, because they fail for unrelated
# reasons and each has its own recovery path:
#
#   read of an existing chunk   - needs its location resolved on demand
#   write to an existing chunk  - needs the same, plus a chunk id to defer on
#   creating a new file         - needs a chunkserver with space to place on
#
# The goal is ec(2,1) across three chunkservers, which raises the bar: placing
# a new chunk needs space on three chunkservers at once, and a chunk is only
# readable once two of its three parts are back.
#
# Everything here is a real chunk on a real disk: the point is to mimic a
# production failover, so every operation must actually be stored and read
# back. The window is stretched with CHUNK_REGISTRATION_BULK_SIZE = 1 and a low
# per-second budget rather than with a large chunk count. The bulk size matters,
# since the default of 1000 would carry every part in the first bulk and close
# the window instantly.

FILE_COUNT=${FILE_COUNT:-100}
FILE_SIZE=${FILE_SIZE:-262144}
REGISTRATION_CHUNKS_PER_SECOND=5
PROBE_COUNT=10
MAX_PROBE_SECONDS=15

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

# One real chunk per file, each stored as 3 ec(2,1) parts over the 3 chunkservers
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

# Bring up the shadow and let it catch up
saunafs_master_n 1 start
assert_eventually "saunafs_shadow_synchronized 1"

# Promote it. The chunkservers keep running, they only reconnect and re-register
# with the new leader. The registration window is paced by the master's budget.
saunafs_master_daemon kill
saunafs_make_conf_for_shadow 0
saunafs_make_conf_for_master 1
saunafs_master_daemon reload

# Count placement failures seen so far, so the probe below can be compared
# against this baseline rather than against zero: earlier phases may
# legitimately have logged some while the cluster was starting up.
placement_errors_before=$(grep -c 'No chunk servers' "$syslog_file" | cat)

# Wait until the new leader has begun learning chunks, and no more than that.
# Deliberately not saunafs_wait_for_all_ready_chunkservers: readiness is the
# post-registration space report, so waiting for it would close the window.
registration_in_progress() {
	local copies
	copies=$(count_chunk_copies)
	[[ -n "$copies" && "$copies" -gt 0 && "$copies" -lt "$FILE_COUNT" ]]
}
assert_eventually 'registration_in_progress'
echo "PHASE: promoted, re-registration window open" \
	"($(count_chunk_copies) / $FILE_COUNT full copies known)"

# Pick targets that are demonstrably still short of the ec(2,1) threshold right
# now, so the reads and writes below really do need their locations resolved.
# They cannot be chosen up front: a chunkserver streams its chunks in its own
# internal order, so "created last" says nothing about "registers last". Reads
# and writes get disjoint sets, because an in-place write replaces part of the
# content that file-validate checks.
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

# Reads of chunks whose parts are not back yet: answered on demand
for probe_file in "${read_targets[@]}"; do
	MESSAGE="reading $(basename "$probe_file") while its parts re-register" \
		assert_success file-validate "$probe_file"
done

# Writes to those same not-yet-registered chunks
for probe_file in "${write_targets[@]}"; do
	MESSAGE="writing to $(basename "$probe_file") while its parts re-register" \
		assert_success dd if=/dev/zero of="$probe_file" bs=4096 count=1 \
			conv=notrunc,fsync status=none
done

# Creating new files: the leader must place three parts on three chunkservers
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

# The IO must have run while re-registration was still going, otherwise this
# test measured a converged cluster. The new files add copies of their own, so
# compare against the seeded total only, which they cannot reach.
assert_less_than "$copies_at_probe" "$FILE_COUNT"

# ...and been served straight away rather than by the client retrying until
# registration finished. Both checks matter: the elapsed time catches a stall,
# and no new placement failure means the leader had space to offer from the
# moment the chunkservers reconnected. A retry loop would show up as both.
assert_less_than "$probe_ms" "$((MAX_PROBE_SECONDS * 1000))"
placement_errors_after=$(grep -c 'No chunk servers' "$syslog_file" | cat)
MESSAGE="client IO must not fall back to the retry loop during a failover" \
	assert_equals "$placement_errors_before" "$placement_errors_after"

# The cluster converges afterwards. The written chunks take longer than the
# rest: their version was bumped while some parts were still unregistered, so
# those arrive stale and are rebuilt in the background rather than simply
# re-registered.
assert_eventually_equals "echo $((FILE_COUNT + PROBE_COUNT))" 'count_chunk_copies' '1 minute'

# Untouched files still validate, and the writes are readable at their new contents
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

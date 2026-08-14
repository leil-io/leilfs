timeout_set '15 minutes'

# Fourth case of the registration-window family: a WRITE to a chunk whose
# registered parts are present but insufficient. Companion to
# test_read_partial_ec_during_registration.sh, which probes the same cluster
# state with a read.
#
# For EC(2,1) any 2 of the 3 parts reconstruct the data, so a chunk with a
# single registered part cannot be written either: the master reports CHUNKLOST
# for it, and before this was handled the client could only exhaust its retry
# budget (sfsioretries) and fail with EIO.
#
# This reaches the same defect as test_write_during_registration.sh through EC
# rather than a replicated goal. The two are kept separate because the write
# path decides writability from whether the known parts can reconstruct the
# chunk, which is EC-aware, while the read path historically keyed on the
# location list being empty, which is not. A future change that makes the write
# side reason about emptiness instead would regress EC while the replicated
# case stayed green; this test is what would catch it.
#
# Note the neighbouring case is NOT a bug and must keep working: at 2 of 3
# parts the chunk is still writable, and with the default REDUNDANCY_LEVEL the
# write proceeds degraded while the missing part is rebuilt in the background.
# Only the 1-of-3 case is probed here.
#
# Only TWO of the three chunkservers are restarted, deliberately. Restarting
# all three would let them re-register roughly in lockstep, so a given chunk
# would spend only a brief racy moment at 1 of 3 parts. Keeping one alive pins
# every chunk at exactly one registered part for the whole window.

FILE_COUNT=${FILE_COUNT:-100}
FILE_SIZE=${FILE_SIZE:-262144}
REGISTRATION_CHUNKS_PER_SECOND=2
PROBE_COUNT=5
MAX_PROBE_SECONDS=10

CHUNKSERVERS=3 \
	DISK_PER_CHUNKSERVER=1 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
	MASTER_CUSTOM_GOALS="10 ec21: \$ec(2,1)" \
	MASTER_EXTRA_CONFIG="CHUNK_REGISTRATION_CHUNKS_PER_SECOND = ${REGISTRATION_CHUNKS_PER_SECOND}|CHUNK_REGISTRATION_BULK_SIZE = 1|CHUNKS_LOOP_TIME = 1|OPERATIONS_DELAY_INIT = 0" \
	CHUNKSERVER_EXTRA_CONFIG="MASTER_RECONNECTION_DELAY = 1" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER,sfsioretries=3" \
	setup_local_empty_saunafs info

file_path() {
	echo "${info[mount0]}/ec21/$(printf 'file_%05d' "$1")"
}

ec_dir="${info[mount0]}/ec21"
mkdir "$ec_dir"
assert_success saunafs setgoal ec21 "$ec_dir"

# One chunk per file, each stored as 3 EC parts spread over the 3 chunkservers
for ((i = 0; i < FILE_COUNT; ++i)); do
	FILE_SIZE=$FILE_SIZE file-generate "$(file_path "$i")"
done

first_file=$(file_path 0)
last_file=$(file_path $((FILE_COUNT - 1)))
MESSAGE="every EC(2,1) chunk must start with all 3 parts known" \
	assert_eventually_equals "echo 3" "count_registered_parts '$last_file'" '3 minutes'
echo "PHASE: $FILE_COUNT EC(2,1) chunks created, 3 parts each"

# Drop every client-side cache so nothing carries over into the probe
saunafs_mount_unmount 0
saunafs_mount_start 0
echo "PHASE: mount restarted, client caches cold"

# Restart 2 of the 3 chunkservers. Chunkserver 0 keeps serving its part, so
# every chunk drops to exactly 1 of 3 registered parts -- present, but one
# short of what EC(2,1) needs.
for csid in 1 2; do
	saunafs_chunkserver_daemon "$csid" restart
done

# Two-sided gate. The first file regaining a second part proves the restarted
# chunkservers have reconnected and are streaming chunks -- they must be
# connected for the on-demand query to have anyone to ask. Deliberately not
# saunafs_wait_for_all_ready_chunkservers, which returns only once registration
# has finished.
restarted_chunkservers_registering() {
	[[ "$(count_registered_parts "$first_file")" -ge 2 ]]
}
assert_eventually 'restarted_chunkservers_registering' '3 minutes'
echo "PHASE: registration window open (first file back to $(count_registered_parts "$first_file") parts)"

# The other side of the gate: pick probe targets that are demonstrably still
# one part short, right now. They cannot be chosen up front -- a chunkserver
# streams its chunks in its own internal order rather than by chunk id, so
# "created last" says nothing about "registers last".
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

# Probe: in-place writes (conv=notrunc keeps the existing chunk instead of
# truncating the file and allocating a fresh one; conv=fsync makes a deferred
# write error surface in dd itself rather than at close) to chunks with a
# single registered part.
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

# Registration finishes and every chunk gets all of its parts back.
#
# Waiting for the chunkservers to report ready is the right tool here, and only
# here: it returns once registration has COMPLETED, which is exactly why it must
# not be used before the probe. Checking every file matters too -- the probe
# targets have their locations resolved by the on-demand query as a side effect
# of being probed, so asserting on one of them would pass without the rest of
# the cluster ever converging.
saunafs_wait_for_all_ready_chunkservers

# Chunks nobody touched must be complete the moment registration finishes:
# their parts only had to be re-registered, not rebuilt.
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

# The probed chunks are a different story: they were written while only one of
# their parts was registered, so the write bumped the chunk version and the
# parts that were still unregistered arrive carrying the old one. Those are
# discarded and rebuilt from the surviving parts in the background, which
# happens after registration rather than as part of it -- so this is an
# eventual check, and it is what proves a write served during the window still
# leaves the chunk able to heal.
probed_chunks_rebuilt() {
	local file
	for file in "${probe_files[@]}"; do
		[[ "$(count_registered_parts "$file")" -eq 3 ]] || return 1
	done
	return 0
}
assert_eventually 'probed_chunks_rebuilt' '5 minutes'

timeout_set '2 minutes'

# Third case of the registration-window family, after
# test_read_during_registration.sh (no part registered) and
# test_write_during_registration.sh (write to an unregistered chunk):
# a chunk whose registered parts are PRESENT BUT INSUFFICIENT to read.
#
# For EC(2,1) any 2 of the 3 parts reconstruct the data, so a chunk with a
# single registered part cannot be read even though its location list is not
# empty. An emptiness check therefore does not describe "cannot be read": the
# two only coincide for replicated goals, where the sole copy is either there
# or not.
#
# Before this was handled, the master answered with the one-part list, never
# issued the on-demand location query (SAU_MATOCS_QUERY_CHUNKS), and left the
# client unable to assemble the data. That failure is retriable, so with a
# generous retry budget it surfaced as a stall of several seconds rather than
# an error, and only became EIO once the retries ran out.
#
# Only TWO of the three chunkservers are restarted, deliberately. Restarting
# all three would let them re-register roughly in lockstep, so a given chunk
# would spend only a brief racy moment at 1 of 3 parts. Keeping one alive pins
# every chunk at exactly one registered part for the whole window.
#
# Progress is tracked per file, with the parts the master currently knows for
# a chunk. The cluster-wide counters cannot express this state: "Chunk copies"
# counts full copies, so a chunk whose parts cannot be assembled contributes 0
# whether one part is registered or none at all, and a chunkserver's reported
# chunk count comes from the space report it sends up front, before any chunk
# is registered.

FILE_COUNT=${FILE_COUNT:-100}
FILE_SIZE=${FILE_SIZE:-262144}
REGISTRATION_CHUNKS_PER_SECOND=5
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
	assert_eventually_equals "echo 3" "count_registered_parts '$last_file'"
echo "PHASE: $FILE_COUNT EC(2,1) chunks created, 3 parts each"

# Drop every client-side cache, so the probe reads below must go to the master
# for chunk locations instead of being served locally
saunafs_mount_unmount 0
saunafs_mount_start 0
echo "PHASE: mount restarted, client caches cold"

# Restart 2 of the 3 chunkservers. Chunkserver 0 keeps serving its part, so
# every chunk drops to exactly 1 of 3 registered parts -- present, but one
# short of what EC(2,1) needs to reconstruct.
for csid in 1 2; do
	saunafs_chunkserver_daemon "$csid" restart
done

# Two-sided gate. The first file re-registers early, so its second registered
# part proves the restarted chunkservers have reconnected and are streaming
# chunks -- they must be connected for the on-demand query to have anyone to
# ask. Deliberately not saunafs_wait_for_all_ready_chunkservers, which returns
# only once registration has finished.
restarted_chunkservers_registering() {
	[[ "$(count_registered_parts "$first_file")" -ge 2 ]]
}
assert_eventually 'restarted_chunkservers_registering'
echo "PHASE: registration window open (first file back to $(count_registered_parts "$first_file") parts)"

# The other side of the gate: pick probe targets that are demonstrably still
# one part short, right now. They cannot be chosen up front: a chunkserver
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
MESSAGE="need $PROBE_COUNT chunks still below the EC(2,1) read threshold; registration outran the probe" \
	assert_equals "$PROBE_COUNT" "${#probe_files[@]}"

# Probe: read files whose chunk has a single registered part. file-validate
# checks the content, not just the exit status, so a short or corrupt read
# cannot pass.
#
# When the request is not held back, these reads do not necessarily fail
# outright: the client treats an unusable location list as retriable and backs
# off, so with a generous retry budget it merely stalls for seconds until
# registration supplies a second part.
# sfsioretries is capped above so the regression surfaces as a clean EIO
# instead of latency that drifts with FILE_COUNT. The elapsed-time bound stays
# as a second line of defence.
probe_start_ns=$(date +%s%N)
for probe_file in "${probe_files[@]}"; do
	MESSAGE="read of EC(2,1) chunk with only 1 of 3 parts registered, $(basename "$probe_file")" \
		assert_success file-validate "$probe_file"
done
probe_ms=$(( ($(date +%s%N) - probe_start_ns) / 1000000 ))

echo "EC_READ_PROBE_MS: $probe_ms for $PROBE_COUNT reads"
assert_less_than "$probe_ms" "$((MAX_PROBE_SECONDS * 1000))"

# Registration finishes and every chunk gets all of its parts back.
#
# Waiting for the chunkservers to report ready is the right tool here, and only
# here: it returns once registration has COMPLETED, which is exactly why it must
# not be used before the probe. Checking every file matters too: the probe targets
# have their locations resolved by the on-demand query as a side effect of being probed,
# so asserting on one of them would pass without the rest of the cluster ever converging.
saunafs_wait_for_all_ready_chunkservers
incomplete=0
for ((i = 0; i < FILE_COUNT; ++i)); do
	[[ "$(count_registered_parts "$(file_path "$i")")" -eq 3 ]] || incomplete=$((incomplete + 1))
done
MESSAGE="every chunk must have all 3 parts registered once the window closes" \
	assert_equals 0 "$incomplete"

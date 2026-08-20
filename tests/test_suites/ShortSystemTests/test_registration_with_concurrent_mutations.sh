timeout_set '2 minutes'

# The resumable pull-registration sweep must not lose chunks when the registry
# mutates mid-sweep: new chunks are inserted and files are unlinked while a
# budget-paced registration is running. What this exercises is the sweep's
# termination pass, which rescans until no unmarked chunk remains and so has to
# pick up chunks inserted after the bucket cursor passed their bucket.
#
# It does NOT exercise a rehash under the cursor, the other hazard that pass
# exists for. The synthetic scan reserves the map for the whole seeded registry
# up front (SyntheticChunkSink::reserve), so at this volume the few dozen
# inserts below cannot grow it past its load factor. That case needs the
# opposite parameters: a tiny seeded registry with many times its size
# inserted.
#
# The window is opened by promoting a shadow, not by restarting chunkservers,
# and that is what makes the premise reachable at all. A restarted chunkserver
# advertises no free space until its registration has COMPLETED, so the leader
# can never choose it to host a new chunk while it sweeps. Every insert would
# land in a registry that had already finished being swept, which is the one
# thing this test must avoid. A chunkserver that merely reconnects scanned its
# disks long ago, reports real space in its registration tail, and can take new
# chunks immediately, while its sweep is still running.
#
# Distinct from test_io_during_failover, which shares the promotion setup but
# asks a different question. That test asks whether client operations succeed
# during the window and bounds their latency. This one asks whether the sweep
# loses anything, so its assertion is the exact chunk count once registration
# has converged: every probe here could succeed instantly and the test would
# still fail if a single chunk went missing. It also needs the volume, a
# large seeded registry and dozens of concurrent inserts and removals for
# the map to rehash under the cursor at all.

CHUNK_COUNT=${CHUNK_COUNT:-100000}
CHUNKS_PER_FILE=1000
REGISTRATION_CHUNKS_PER_SECOND=10000

MASTERSERVERS=2 \
	CHUNKSERVERS=2 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
	MASTER_EXTRA_CONFIG="CHUNK_REGISTRATION_CHUNKS_PER_SECOND = ${REGISTRATION_CHUNKS_PER_SECOND}" \
	CHUNKSERVER_EXTRA_CONFIG="MASTER_RECONNECTION_DELAY = 1" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

syslog_file="${ERROR_DIR}/syslog.log"

# Seed metadata offline; disjoint id ranges per chunkserver at goal 1
saunafs_master_daemon stop
metadata_version=$(metadata_get_version "${info[master_data_path]}/metadata.sfs")
generate_mock_chunks_changelog "$((2 * CHUNK_COUNT))" "$CHUNKS_PER_FILE" "$metadata_version" \
	> "${info[master_data_path]}/changelog.sfs"
assert_success sfsmetarestore -a -d "${info[master_data_path]}"
saunafs_master_daemon start
saunafs_wait_for_all_ready_chunkservers

for csid in 0 1; do
	mock_dir="$RAMDISK_DIR/mock_disk_$csid"
	mkdir -p "$mock_dir"
	echo "mock:${CHUNK_COUNT}:$((csid * CHUNK_COUNT + 1)):${mock_dir}" \
		>> "${info[chunkserver${csid}_hdd]}"
	saunafs_chunkserver_daemon "$csid" reload
done

assert_eventually_equals "echo $((2 * CHUNK_COUNT))" 'count_chunk_copies'
echo "PHASE: initial registration complete"

# Bring up the shadow and let it catch up
saunafs_master_n 1 start
assert_eventually "saunafs_shadow_synchronized 1"

# Sweeps completed by the master promoted below, counted on their own rather
# than as a delta against a snapshot taken here. Syslog delivery lags the
# script by up to a second, so a snapshot can miss registrations that have
# already completed: the master restart during the seeding above produces
# two, and they then surface later as if they had happened during the
# mutations. Attributing them to the promoted master instead needs no
# snapshot: before the promotion its count is necessarily zero.
sweeps_finished_after_promotion() {
	grep -c "master_1\[.*finished master-driven chunk registration" "$syslog_file" | cat
}

# Promote the shadow. The chunkservers keep running, reconnect to the new
# leader and re-register everything, paced by the master's budget.
saunafs_master_daemon kill
saunafs_make_conf_for_shadow 0
saunafs_make_conf_for_master 1
saunafs_master_daemon reload

# Wait only until the sweep has STARTED, while it is still far from complete.
# Deliberately not saunafs_wait_for_all_ready_chunkservers: readiness is the
# post-registration space report, so waiting for it would close the window the
# mutations below are supposed to run concurrently with.
registration_in_progress() {
	local copies
	copies=$(count_chunk_copies)
	[[ -n "$copies" && "$copies" -gt 0 && "$copies" -lt $((2 * CHUNK_COUNT)) ]]
}
assert_eventually 'registration_in_progress'
echo "PHASE: promoted, paced re-registration running" \
	"($(count_chunk_copies) / $((2 * CHUNK_COUNT)) copies known)"

# Mutations during the sweep: create many new files (their chunks land on the
# real disks of chunkservers that are still streaming, so the inserts rehash a
# registry that is being swept), read them back, and unlink a seeded mock file
# plus some of the new files
created=0
for i in $(seq 1 40); do
	echo "mutation data $i" > "${info[mount0]}/mutation_$i"
	created=$((created + 1))
done
for i in $(seq 1 40); do
	MESSAGE="reading back mutation_$i during registration" \
		assert_equals "mutation data $i" "$(cat "${info[mount0]}/mutation_$i")"
done
assert_success rm -f "${info[mount0]}/mock_0000001"
for i in $(seq 31 40); do
	assert_success rm -f "${info[mount0]}/mutation_$i"
	created=$((created - 1))
done

# The mutations must have run while the sweep was still going, otherwise they
# were concurrent with nothing. The new files add at most a few dozen copies of
# their own, far short of closing the gap to the full mock total, so comparing
# against it stays a safe lower bound on "still registering".
copies_after_mutations=$(count_chunk_copies)
MESSAGE="mutations must run while the registration sweep is still going" \
	assert_less_than "$copies_after_mutations" "$((2 * CHUNK_COUNT))"

# Stronger: no chunkserver finished its sweep while the mutations ran, so every
# insert above necessarily landed in a registry that was still being swept.
# Without this the test can quietly degrade to mutating an already-finished
# chunkserver, which is what it looked like before it was promotion-based.
MESSAGE="inserts must land in a registry that is still being swept" \
	assert_equals 0 "$(sweeps_finished_after_promotion)"

echo "PHASE: mutations done at $copies_after_mutations / $((2 * CHUNK_COUNT)) copies," \
	"waiting for convergence"

# Convergence: every remaining mock copy plus every remaining new chunk is
# known (>=: copies of the unlinked chunks disappear later via the regular
# deletion machinery)
expected_min=$((2 * CHUNK_COUNT - CHUNKS_PER_FILE + created))
copies_at_least() {
	local copies
	copies=$(count_chunk_copies)
	[[ -n "$copies" && "$copies" -ge "$expected_min" ]]
}
assert_eventually 'copies_at_least' '30 seconds'

# The removed seeded file can remain in the global copy count while the
# deletion queue settles, so it cannot prove that every retained mutation was
# registered. fileinfo has no on-demand deferral path, making this a direct
# check of the master's registration state for all surviving new chunks.
surviving_mutations_registered() {
	local i
	for ((i = 1; i <= created; ++i)); do
		[[ "$(count_registered_parts "${info[mount0]}/mutation_$i")" -eq 1 ]] || return 1
	done
	return 0
}
MESSAGE="every surviving mutation must be registered after the sweep" \
	assert_eventually 'surviving_mutations_registered' '30 seconds'

# Data still consistent afterwards
expected_head="5a5b58595e5f5c5d5253505156575455"
assert_equals "$expected_head" \
	"$(dd if="${info[mount0]}/mock_0000002" bs=16 count=1 2>/dev/null | od -An -tx1 | tr -d ' \n')"
assert_equals "mutation data 1" "$(cat "${info[mount0]}/mutation_1")"

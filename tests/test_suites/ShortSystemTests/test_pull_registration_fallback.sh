# CHUNK_REGISTRATION_FORCE_PUSH=1 must keep a chunkserver on the old push
# registration protocol even against a pull-capable master, and everything
# must still converge. This also covers the mixed-version path (an old
# chunkserver never enters the pull protocol).
#
# A registration budget is configured deliberately. The master decides pull
# mode from the chunkserver's version alone, so a forced-push chunkserver is
# marked pull-mode and then never sends the END that closes the connection
# out. Left uncorrected the master treats it as registering forever, drawing
# on the shared budget for every bulk and paying credits into a stream that
# ignores them, which starves the chunkservers that really are paced by it.
# None of that is visible without a budget set, since the unbudgeted path
# grants credits without consulting any counter.

CHUNK_COUNT=${CHUNK_COUNT:-100000}
CHUNKS_PER_FILE=1000
REGISTRATION_CHUNKS_PER_SECOND=50000

MASTERSERVERS=1 \
	CHUNKSERVERS=2 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
	MASTER_EXTRA_CONFIG="CHUNK_REGISTRATION_CHUNKS_PER_SECOND = ${REGISTRATION_CHUNKS_PER_SECOND}" \
	CHUNKSERVER_EXTRA_CONFIG="MASTER_RECONNECTION_DELAY = 1|CHUNK_REGISTRATION_FORCE_PUSH = 1" \
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

# Restart both chunkservers: re-registration must also use push
for csid in 0 1; do
	saunafs_chunkserver_daemon "$csid" restart
done
saunafs_wait_for_all_ready_chunkservers
assert_eventually_equals "echo $((2 * CHUNK_COUNT))" 'count_chunk_copies'

# IO works
expected_head="5a5b58595e5f5c5d5253505156575455"
assert_equals "$expected_head" \
	"$(dd if="${info[mount0]}/mock_0000002" bs=16 count=1 2>/dev/null | od -An -tx1 | tr -d ' \n')"
echo "canary" > "${info[mount0]}/canary"
assert_equals "canary" "$(cat "${info[mount0]}/canary")"

# The pull protocol was never entered by the forced-push chunkservers
assert_equals 0 "$(grep -c 'master-driven chunk registration started' "$syslog_file" | cat)"

# Every registration that began was also closed out, rather than left
# registering forever. Counted against the registrations the master actually
# saw rather than a fixed number: chunkservers register more often than the
# restart above suggests, the master restart during seeding costing a round of
# its own. The ' - ip:' anchor keeps this from matching the assertion text that
# a failure writes into the same log.
#
# Without the fix the forced-push connections never reach this state, so the
# master keeps granting them credits and charging the shared budget for a
# stream that ignores both.
registrations_begun() {
	grep -c 'chunkserver register begin' "$syslog_file" | cat
}

registrations_closed() {
	grep -c 'finished master-driven chunk registration - ip:' "$syslog_file" | cat
}

every_registration_closed() {
	local begun closed
	begun=$(registrations_begun)
	closed=$(registrations_closed)
	[[ "$begun" -gt 0 && "$begun" -eq "$closed" ]]
}
MESSAGE="the master must close out a forced-push registration, not wait for an END that never comes" \
	assert_eventually 'every_registration_closed'

# A chunkserver that registers by push must be able to take on new chunks
# afterwards: a connection stuck mid-registration keeps drawing on the budget,
# so writes here would slow down or stall behind it.
for i in $(seq 1 20); do
	echo "post-registration $i" > "${info[mount0]}/post_$i"
done

for i in $(seq 1 20); do
	MESSAGE="reading back post_$i written after a forced-push registration" \
		assert_equals "post-registration $i" "$(cat "${info[mount0]}/post_$i")"
done

# Verify forced push against a pull-capable master, including after restarts.
# Enable a pull budget to check credit cleanup and registration completion.

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

# Seed goal-1 metadata with disjoint chunk IDs for each server.
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

# Re-registration must also use push.
for csid in 0 1; do
	saunafs_chunkserver_daemon "$csid" restart
done

saunafs_wait_for_all_ready_chunkservers
assert_eventually_equals "echo $((2 * CHUNK_COUNT))" 'count_chunk_copies'

# Verify existing mock data and a new real-file round trip.
expected_head="5a5b58595e5f5c5d5253505156575455"
assert_equals "$expected_head" \
	"$(dd if="${info[mount0]}/mock_0000002" bs=16 count=1 2>/dev/null | od -An -tx1 | tr -d ' \n')"

echo "canary" > "${info[mount0]}/canary"
assert_equals "canary" "$(cat "${info[mount0]}/canary")"

# Forced-push servers must never enter the pull protocol.
assert_equals 0 "$(grep -c 'master-driven chunk registration started' "$syslog_file" | cat)"

# Every registration must close, including reconnects during metadata seeding.
registrations_begun() {
	grep -c 'chunkserver register begin' "$syslog_file" | cat
}

registrations_closed() {
	# Include ' - ip:' to avoid counting assertion text in syslog.
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

# Verify push-registered chunkservers accept new chunks and preserve data.
for i in $(seq 1 20); do
	echo "post-registration $i" > "${info[mount0]}/post_$i"
done

for i in $(seq 1 20); do
	MESSAGE="reading back post_$i written after a forced-push registration" \
		assert_equals "post-registration $i" "$(cat "${info[mount0]}/post_$i")"
done

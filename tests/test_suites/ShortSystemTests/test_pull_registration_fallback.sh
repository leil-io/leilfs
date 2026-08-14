timeout_set '10 minutes'

# CHUNK_REGISTRATION_FORCE_PUSH=1 must keep a chunkserver on the old push
# registration protocol even against a pull-capable master, and everything
# must still converge. This also covers the mixed-version path (an old
# chunkserver never enters the pull protocol).

CHUNK_COUNT=${CHUNK_COUNT:-100000}
CHUNKS_PER_FILE=1000

MASTERSERVERS=1 \
	CHUNKSERVERS=2 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
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

assert_eventually_equals "echo $((2 * CHUNK_COUNT))" 'count_chunk_copies' '5 minutes'

# Restart both chunkservers: re-registration must also use push
for csid in 0 1; do
	saunafs_chunkserver_daemon "$csid" restart
done
saunafs_wait_for_all_ready_chunkservers
assert_eventually_equals "echo $((2 * CHUNK_COUNT))" 'count_chunk_copies' '5 minutes'

# IO works
expected_head="5a5b58595e5f5c5d5253505156575455"
assert_equals "$expected_head" \
	"$(dd if="${info[mount0]}/mock_0000002" bs=16 count=1 2>/dev/null | od -An -tx1 | tr -d ' \n')"
echo "canary" > "${info[mount0]}/canary"
assert_equals "canary" "$(cat "${info[mount0]}/canary")"

# The pull protocol was never entered by the forced-push chunkservers
assert_equals 0 "$(grep -c 'master-driven chunk registration started' "$syslog_file" | cat)"

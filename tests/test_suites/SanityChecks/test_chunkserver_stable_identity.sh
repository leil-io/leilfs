timeout_set 120 seconds

uuid_pattern='^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$'
chunkserver_log="${TEMP_DIR}/chunkserver.log"

CHUNKSERVERS=2 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	CHUNKSERVER_EXTRA_CONFIG="MAGIC_DEBUG_LOG=${chunkserver_log}|LOG_FLUSH_ON=INFO" \
	setup_local_empty_saunafs info

chunkserver0_data_path=$(sed -n 's/^DATA_PATH = //p' "${info[chunkserver0_cfg]}")
chunkserver1_data_path=$(sed -n 's/^DATA_PATH = //p' "${info[chunkserver1_cfg]}")
chunkserver0_id_file="${chunkserver0_data_path}/chunkserver_id"
chunkserver1_id_file="${chunkserver1_data_path}/chunkserver_id"

assert_file_exists "${chunkserver0_id_file}"
assert_file_exists "${chunkserver1_id_file}"

chunkserver0_id=$(<"${chunkserver0_id_file}")
chunkserver1_id=$(<"${chunkserver1_id_file}")
assert_matches "${uuid_pattern}" "${chunkserver0_id}"
assert_matches "${uuid_pattern}" "${chunkserver1_id}"
assert_not_equal "${chunkserver0_id}" "${chunkserver1_id}"

cd "${info[mount0]}"
FILE_SIZE=1M file-generate file
file-validate file

saunafs_chunkserver_daemon 0 restart
saunafs_wait_for_all_ready_chunkservers

assert_equals "${chunkserver0_id}" "$(<"${chunkserver0_id_file}")"
# Both starts of chunkserver 0 must have logged the same identity.
assert_equals 2 "$(grep -cF "chunkserver identity: ${chunkserver0_id}" "${chunkserver_log}")"
file-validate file

previous_chunkserver0_id="${chunkserver0_id}"

# Commands which do not start the service must not create an identity.
saunafs_chunkserver_daemon 0 stop
rm "${chunkserver0_id_file}"
assert_success saunafs_chunkserver_daemon 0 test
assert_failure saunafs_chunkserver_daemon 0 isalive
assert_file_not_exists "${chunkserver0_id_file}"

saunafs_chunkserver_daemon 0 start
saunafs_wait_for_all_ready_chunkservers
chunkserver0_id=$(<"${chunkserver0_id_file}")
assert_matches "${uuid_pattern}" "${chunkserver0_id}"
assert_not_equal "${previous_chunkserver0_id}" "${chunkserver0_id}"

# An invalid identity must stop startup without silently replacing the file.
saunafs_chunkserver_daemon 0 stop
printf 'invalid\n' >"${chunkserver0_id_file}"
assert_failure saunafs_chunkserver_daemon 0 start
assert_equals invalid "$(<"${chunkserver0_id_file}")"

# Failure to persist a new identity must stop startup.
rm "${chunkserver0_id_file}"
mkdir "${chunkserver0_id_file}.tmp"
assert_failure saunafs_chunkserver_daemon 0 start
assert_file_not_exists "${chunkserver0_id_file}"
assert_success test -d "${chunkserver0_id_file}.tmp"
rmdir "${chunkserver0_id_file}.tmp"

printf '%s\n' "${chunkserver0_id}" >"${chunkserver0_id_file}"
saunafs_chunkserver_daemon 0 start
saunafs_wait_for_all_ready_chunkservers
file-validate file

timeout_set 10 minutes

USE_RAMDISK=YES \
	MOUNTS=1 \
	CHUNKSERVERS=1 \
	DISK_PER_CHUNKSERVER=2 \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	MASTER_EXTRA_CONFIG="CHUNKS_LOOP_MIN_TIME = 1`
			`|CHUNKS_LOOP_MAX_CPU = 90`
			`|OPERATIONS_DELAY_INIT = 0" \
	CHUNKSERVER_EXTRA_CONFIG="CHUNK_TRASH_ENABLED=1|`
			`CHUNK_TRASH_EXPIRATION_SECONDS=50" \
	setup_local_empty_saunafs info

create_test_file() {
	local file_path="$1"

	touch "${file_path}"
	dd if=/dev/zero of="${file_path}" bs=1MiB count=130
	saunafs settrashtime 0 "${file_path}"
}

disable_chunkserver_disk_in_config_only() {
	local chunkserver_id="$1"
	local disk_id="$2"
	local hdd_file="${saunafs_info_[chunkserver${chunkserver_id}_hdd]}"
	local line_num=$((disk_id + 1))

	sed -i "${line_num}s/^/# /" "${hdd_file}"

	echo "Disabled disk ${disk_id} for chunkserver ${chunkserver_id}"
}

get_chunkserver_disk_cfg_line() {
	local chunkserver_id="$1"
	local disk_id="$2"
	local hdd_file="${saunafs_info_[chunkserver${chunkserver_id}_hdd]}"
	local line_num=$((disk_id + 1))

	sed -n "${line_num}p" "${hdd_file}"
}

get_chunkserver_disk_base_path() {
	local chunkserver_id="$1"
	local disk_id="$2"
	local cfg_line

	cfg_line="$(get_chunkserver_disk_cfg_line "${chunkserver_id}" "${disk_id}")"

	printf '%s\n' "${cfg_line}" | sed -E \
		-e 's/^[[:space:]]*#\s*//' \
		-e 's/\*//g' \
		-e 's/^zonefs://' \
		-e 's/\|.*$//' \
		-e 's@[[:space:]]+$@@'
}

get_chunkserver_disk_trash_path() {
	local chunkserver_id="$1"
	local disk_id="$2"
	local disk_path

	disk_path="$(get_chunkserver_disk_base_path "${chunkserver_id}" "${disk_id}")"
	printf '%s/.trash.bin\n' "${disk_path%/}"
}

count_trashed_chunks_in_path() {
	local trash_path="$1"
	local chunk_metadata_pattern="chunk*${chunk_metadata_extension}.*"
	local chunk_data_pattern="chunk*${chunk_data_extension}.*"

	if [[ ! -d "${trash_path}" ]]; then
		echo 0
		return
	fi

	find "${trash_path}" \
		\( -name "${chunk_data_pattern}" -o -name "${chunk_metadata_pattern}" \) \
		2>/dev/null | wc -l
}

count_trashed_chunks_on_disk() {
	local chunkserver_id="$1"
	local disk_id="$2"
	local trash_path

	trash_path="$(get_chunkserver_disk_trash_path "${chunkserver_id}" "${disk_id}")"
	count_trashed_chunks_in_path "${trash_path}"
}

assert_trashed_chunks_on_disk() {
	local expected_op="$1"
	local expected_value="$2"
	local chunkserver_id="$3"
	local disk_id="$4"
	local timeout="$5"
	local message="$6"

	if ! wait_for "[[ \$(count_trashed_chunks_on_disk ${chunkserver_id} ${disk_id}) ${expected_op} ${expected_value} ]]" "${timeout}"; then
		local current_count
		current_count="$(count_trashed_chunks_on_disk "${chunkserver_id}" "${disk_id}")"
		test_add_failure "${message}"$'\n'"Current trashed chunk count on disk ${disk_id}: ${current_count}"
	fi
}

chunkserver_id=0
missing_disk_id=0
other_disk_id=1

cd "${info[mount0]}"

######################################################################
#### Put some trashed chunks on a specific disk
######################################################################

# Disable one disk so new chunks are placed on the disk that will later
# be removed from the HDD config.
disable_chunkserver_disk "${chunkserver_id}" "${other_disk_id}"

# Wait for CS to actually remove the disk.
sleep 10

mkdir goal1
saunafs setgoal 1 goal1

# Create a couple of files and remove them so their chunks go to trash.
create_test_file "${info[mount0]}/goal1/file_1"
create_test_file "${info[mount0]}/goal1/file_2"

rm -f "${info[mount0]}/goal1/file_1" "${info[mount0]}/goal1/file_2"

# Wait for all live chunks to disappear.
if ! wait_for '[[ $(find_all_chunks | wc -l) == 0 ]]' "3 minutes"; then
	test_add_failure $'The following chunks were not removed:\n'"$(find_all_chunks)"
fi

# Verify that the future-missing disk contains trashed chunks.
trashed_chunks_before_stop="$(count_trashed_chunks_on_disk "${chunkserver_id}" "${missing_disk_id}")"
echo "Trashed chunks on disk ${missing_disk_id} before stop: ${trashed_chunks_before_stop}"

MESSAGE="Expected trashed chunks on the disk that will be missing after restart" \
assert_success test "${trashed_chunks_before_stop}" -gt 0

# Restore the original disk set before stopping the chunkserver.
reenable_chunkserver_disk "${chunkserver_id}" "${other_disk_id}"

######################################################################
#### Stop chunkserver and start it without one of the original disks
######################################################################

# Gracefully stop the chunkserver.
saunafs_chunkserver_daemon "${chunkserver_id}" stop

# Remove the disk from HDD config while the chunkserver is stopped.
disable_chunkserver_disk_in_config_only "${chunkserver_id}" "${missing_disk_id}"

# Start the chunkserver without the disk that still contains trashed files.
saunafs_chunkserver_daemon "${chunkserver_id}" start

# Wait until the trash expiration time passes while the disk is absent.
sleep 60

# Verify that the expired trashed chunks are still present on the missing disk.
trashed_chunks_while_missing="$(count_trashed_chunks_on_disk "${chunkserver_id}" "${missing_disk_id}")"
echo "Trashed chunks on disk ${missing_disk_id} while missing: ${trashed_chunks_while_missing}"

MESSAGE="Expected trashed chunks to remain on the missing disk while it is absent from config" \
assert_success test "${trashed_chunks_while_missing}" -gt 0

######################################################################
#### Re-add the missing disk with reload and verify pending trash is purged
######################################################################

# Re-add the disk and reload the chunkserver configuration.
reenable_chunkserver_disk "${chunkserver_id}" "${missing_disk_id}"

# Verify that the re-added disk is scanned and its expired trashed chunks
# are eventually removed after reload.
assert_trashed_chunks_on_disk "==" 0 "${chunkserver_id}" "${missing_disk_id}" "3 minutes" \
	$'The trashed chunks on the re-added disk were not removed after reload'

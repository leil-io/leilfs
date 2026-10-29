timeout_set '4 minutes'

# After promotion, a write to a goal-2 chunk whose second copy sits on a connected
# chunkserver that has not reported it yet must leave both copies current once that
# chunkserver finishes registering, and must survive the loss of the other copy.

# Keep name storage consistent when promotion reloads the generated configs.
create_bdb_name_storage_entry_() {
	echo 'USE_BDB_FOR_NAME_STORAGE = 0'
}

MASTERSERVERS=2 \
	CHUNKSERVERS=2 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
	MASTER_EXTRA_CONFIG="CHUNK_REGISTRATION_CHUNKS_PER_SECOND = 100`
			`|CHUNK_REGISTRATION_BULK_SIZE = 1`
			`|CHUNK_REGISTRATION_WINDOW = 1" \
	CHUNKSERVER_EXTRA_CONFIG="MASTER_RECONNECTION_DELAY = 1" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER,sfsioretries=3" \
	setup_local_empty_saunafs info

# One chunk with a copy on each chunkserver.
target_dir="${info[mount0]}/target_dir"
mkdir "${target_dir}"
assert_success saunafs setgoal 2 "${target_dir}"
target_file="${target_dir}/target"
FILE_SIZE=1M file-generate "${target_file}"
assert_eventually_equals 'echo 2' "count_registered_parts '${target_file}'"

expected_file="${TEMP_DIR}/expected"
cp "${target_file}" "${expected_file}"
dd if=/dev/zero of="${expected_file}" bs=4096 count=1 conv=notrunc status=none

echo "BEFORE PROMOTION:"
saunafs fileinfo "${target_file}"

# Synchronize the shadow, take chunkserver 1 down, promote.
saunafs_master_n 1 start
assert_eventually "saunafs_shadow_synchronized 1"
saunafs_chunkserver_daemon 1 stop
saunafs_master_daemon kill
saunafs_make_conf_for_shadow 0
saunafs_make_conf_for_master 1
saunafs_master_daemon reload

# Chunkserver 1 comes back through a proxy that withholds every registration
# credit: connected and in paced registration, but none of its chunks reported.
get_next_port_number proxy_port
ready_marker="${TEMP_DIR}/credit-gate-ready"
held_marker="${TEMP_DIR}/credits-held"
release_marker="${TEMP_DIR}/release-credits"
end_marker="${TEMP_DIR}/registration-ended"
proxy_log="${TEMP_DIR}/credit-gate.log"

python3 "${SOURCE_DIR}/tests/tools/registration_credit_gate.py" \
	--host "$(get_ip_addr)" --listen-port "${proxy_port}" --master-port "${info[matocs]}" \
	--initial-credits 0 \
	--ready-marker "${ready_marker}" --held-marker "${held_marker}" \
	--release-marker "${release_marker}" --end-marker "${end_marker}" \
	> "${proxy_log}" 2>&1 &
proxy_pid=$!
trap 'kill "${proxy_pid}" 2>/dev/null || true; wait "${proxy_pid}" 2>/dev/null || true' EXIT
assert_eventually 'test -f "${ready_marker}"'

add_lines_sfschunkserver_cfg_ "MASTER_HOST = $(get_ip_addr)|MASTER_PORT = ${proxy_port}" 1
saunafs_chunkserver_daemon 1 start
assert_eventually 'test -f "${held_marker}"'

# Premise: the master knows only chunkserver 0's copy while chunkserver 1 registers.
MESSAGE='premise: only chunkserver 0 has reported its copy' \
	assert_eventually_equals 'echo 1' "count_registered_parts '${target_file}'"

echo "AFTER PROMOTION, BEFORE WRITE:"
saunafs fileinfo "${target_file}"

MESSAGE='the write after promotion must succeed' \
	assert_success dd if=/dev/zero of="${target_file}" bs=4096 count=1 \
		conv=notrunc,fsync status=none

echo "AFTER WRITE:"
saunafs fileinfo "${target_file}"

# Let chunkserver 1 finish registering; its bulk precedes END on the same connection.
touch "${release_marker}"
assert_eventually 'test -f "${end_marker}"'

echo "AFTER CHUNKSERVER 1 REGISTERED:"
saunafs fileinfo "${target_file}"
cat "${proxy_log}"

MESSAGE='the copy reported after the write must be current' \
	expect_equals 2 "$(count_registered_parts "${target_file}")"

# Consequence: losing chunkserver 0 must not lose the data written after promotion.
saunafs_chunkserver_daemon 0 stop
MESSAGE='data written after promotion must survive the loss of chunkserver 0' \
	assert_success cmp "${expected_file}" "${target_file}"

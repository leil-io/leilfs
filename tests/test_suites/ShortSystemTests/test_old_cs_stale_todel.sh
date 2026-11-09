timeout_set '4 minutes'

# A legacy chunkserver must stay connected when a disk-removal report arrives
# after a version change. Use real 4.1.0 services, as in the upgrade tests.

CHUNKSERVERS=1 \
	DISK_PER_CHUNKSERVER=1 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	START_WITH_LEGACY_SAUNAFS=YES \
	CHUNKSERVER_EXTRA_CONFIG="MASTER_RECONNECTION_DELAY = 1" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

# New master, old chunkserver and old mount.
saunafs_master_daemon restart
saunafs_wait_for_all_ready_chunkservers

target_dir="${info[mount0]}/target_dir"
mkdir "${target_dir}"
assert_success saunafs setgoal 1 "${target_dir}"
target_file="${target_dir}/target"
FILE_SIZE=1M file-generate "${target_file}"
assert_eventually_equals 'echo 1' "count_registered_parts '${target_file}'"

expected_file="${TEMP_DIR}/expected"
cp "${target_file}" "${expected_file}"
dd if=/dev/zero of="${expected_file}" bs=4096 count=1 conv=notrunc status=none

chunk_name=$(saunafs fileinfo "${target_file}" | awk '/chunk 0:/ {print $3; exit}')
assert_success test -n "${chunk_name}"
target_chunk=${chunk_name%%_*}
initial_version=$((16#${chunk_name##*_}))

# Route the old chunkserver through the proxy and let it re-register.
get_next_port_number proxy_port
ready_marker="${TEMP_DIR}/proxy-ready"
held_marker="${TEMP_DIR}/report-held"
release_marker="${TEMP_DIR}/release-report"
delivered_marker="${TEMP_DIR}/report-delivered"
master_closed_marker="${TEMP_DIR}/master-closed"
proxy_log="${TEMP_DIR}/stale-todel-proxy.log"

python3 "${SOURCE_DIR}/tests/tools/registration_stale_todel_proxy.py" \
	--host "$(get_ip_addr)" --listen-port "${proxy_port}" --master-port "${info[matocs]}" \
	--target-chunk "0x${target_chunk}" \
	--ready-marker "${ready_marker}" --held-marker "${held_marker}" \
	--release-marker "${release_marker}" --delivered-marker "${delivered_marker}" \
	--master-closed-marker "${master_closed_marker}" \
	> "${proxy_log}" 2>&1 &
proxy_pid=$!
trap 'kill "${proxy_pid}" 2>/dev/null || true; wait "${proxy_pid}" 2>/dev/null || true' EXIT
assert_eventually 'test -f "${ready_marker}"'

add_lines_sfschunkserver_cfg_ "MASTER_HOST = $(get_ip_addr)|MASTER_PORT = ${proxy_port}" 0
saunafsXX_chunkserver_daemon 0 restart
assert_eventually 'grep -q "connection 1 accepted" "${proxy_log}"'
assert_eventually_equals 'echo 1' "count_registered_parts '${target_file}'"

# Mark the only disk for removal: the old chunkserver re-announces the chunk with
# the to-delete flag at its current version; the proxy holds that announcement.
sed -i '1s/^/*/' "${info[chunkserver0_hdd]}"
saunafsXX_chunkserver_daemon 0 reload
MESSAGE='premise: the old chunkserver re-announces the chunk with the to-delete flag' \
	assert_eventually 'test -f "${held_marker}"' '30 seconds'

# Raise the chunk version while the announcement is held.
MESSAGE='the write must succeed' \
	assert_success dd if=/dev/zero of="${target_file}" bs=4096 count=1 \
		conv=notrunc,fsync status=none
current_chunk=$(saunafs fileinfo "${target_file}" | awk '/chunk 0:/ {print $3; exit}')
MESSAGE='premise: the write raised the chunk version once' \
	assert_equals "$((initial_version + 1))" "$((16#${current_chunk##*_}))"

# Deliver the stale announcement: older version, different to-delete flag.
touch "${release_marker}"
assert_eventually 'test -f "${delivered_marker}"'

master_closed_within() {
	local deadline=$((SECONDS + ${1}))
	while ((SECONDS < deadline)); do
		test -f "${master_closed_marker}" && return 0
		sleep 0.2
	done
	return 1
}

cat "${proxy_log}"
MESSAGE='the master must not disconnect an older chunkserver over a stale report' \
	expect_failure master_closed_within 15
cat "${proxy_log}"

MESSAGE='the stale report must preserve the current copy and overwritten data' \
	assert_equals 1 "$(count_registered_parts "${target_file}")"
assert_files_equal "${expected_file}" "${target_file}"

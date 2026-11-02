timeout_set '3 minutes'

# Query missing copies while registration is incomplete, including completed
# peers. Once every inventory finishes, unreadable chunks must not start queries.

CHUNKSERVERS=2 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	MASTER_EXTRA_CONFIG="CHUNK_REGISTRATION_CHUNKS_PER_SECOND = 100000" \
	CHUNKSERVER_EXTRA_CONFIG="MASTER_RECONNECTION_DELAY = 1" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER,sfsioretries=1" \
	setup_local_empty_saunafs info

# The only copy of the file lives on chunkserver 0.
saunafs_chunkserver_daemon 1 stop

target_dir="${info[mount0]}/target_dir"
mkdir "${target_dir}"
assert_success saunafs setgoal 1 "${target_dir}"

target_file="${target_dir}/target"
FILE_SIZE=1M file-generate "${target_file}"
assert_eventually_equals 'echo 1' "count_registered_parts '${target_file}'"

# Observe query replies from chunkserver 1 without holding its inventory.
get_next_port_number proxy_port

ready_marker="${TEMP_DIR}/gate-ready"
held_marker="${TEMP_DIR}/gate-held"
release_marker="${TEMP_DIR}/gate-release"
end_marker="${TEMP_DIR}/gate-end"
query_marker="${TEMP_DIR}/query-reply-forwarded"
proxy_log="${TEMP_DIR}/gate.log"

touch "${release_marker}"
python3 "${SOURCE_DIR}/tests/tools/registration_credit_gate.py" \
	--host "$(get_ip_addr)" \
	--listen-port "${proxy_port}" \
	--master-port "${info[matocs]}" \
	--ready-marker "${ready_marker}" \
	--held-marker "${held_marker}" \
	--release-marker "${release_marker}" \
	--end-marker "${end_marker}" \
	--query-marker "${query_marker}" \
	> "${proxy_log}" 2>&1 &
proxy_pid=$!

trap '
	kill "${proxy_pid}" 2>/dev/null || true
	wait "${proxy_pid}" 2>/dev/null || true
' EXIT

assert_eventually 'test -f "${ready_marker}"'

add_lines_sfschunkserver_cfg_ "MASTER_HOST = $(get_ip_addr)|MASTER_PORT = ${proxy_port}" 1
saunafs_chunkserver_daemon 1 start

# Begin with both inventories complete and no observed queries.
MESSAGE='premise: chunkserver 1 finished registering' \
	assert_eventually 'test -f "${end_marker}"'
wait_for_chunkserver_pull_registration 0
assert_failure test -f "${query_marker}"

# Restart the only holder while withholding its registration credits.
saunafs_chunkserver_daemon 0 stop
assert_eventually_equals 'echo 0' "count_registered_parts '${target_file}'"

get_next_port_number recovery_port

recovery_ready="${TEMP_DIR}/recovery-ready"
recovery_held="${TEMP_DIR}/recovery-held"
recovery_release="${TEMP_DIR}/recovery-release"
recovery_end="${TEMP_DIR}/recovery-end"
recovery_log="${TEMP_DIR}/recovery-gate.log"

python3 "${SOURCE_DIR}/tests/tools/registration_credit_gate.py" \
	--host "$(get_ip_addr)" \
	--listen-port "${recovery_port}" \
	--master-port "${info[matocs]}" \
	--initial-credits 0 \
	--ready-marker "${recovery_ready}" \
	--held-marker "${recovery_held}" \
	--release-marker "${recovery_release}" \
	--end-marker "${recovery_end}" \
	> "${recovery_log}" 2>&1 &
recovery_proxy_pid=$!

trap '
	for pid in "${proxy_pid}" "${recovery_proxy_pid}"; do
		kill "${pid}" 2>/dev/null || true
		wait "${pid}" 2>/dev/null || true
	done
' EXIT

assert_eventually 'test -f "${recovery_ready}"'

add_lines_sfschunkserver_cfg_ "MASTER_HOST = $(get_ip_addr)|MASTER_PORT = ${recovery_port}" 0
saunafs_chunkserver_daemon 0 start

assert_eventually 'test -f "${recovery_held}"'
assert_failure test -f "${recovery_end}"
assert_equals 0 "$(count_registered_parts "${target_file}")"

# Clear client caches so success requires discovery of the unreported copy.
saunafs_mount_unmount 0
saunafs_mount_start 0

assert_success file-validate "${target_file}"

# Discovery must also query completed peers while another inventory is open.
MESSAGE='recovery reads must query completed peers too' \
	assert_success test -f "${query_marker}"
assert_failure test -f "${recovery_end}"

# Finish registration and clear the query marker before the steady-state check.
touch "${recovery_release}"
assert_eventually 'test -f "${recovery_end}"'
assert_success kill -0 "${recovery_proxy_pid}"

rm "${query_marker}"

# Disconnect the only holder, then read with empty client caches.
saunafs_chunkserver_daemon 0 stop
assert_eventually_equals 'echo 0' "count_registered_parts '${target_file}'"

saunafs_mount_unmount 0
saunafs_mount_start 0

start_ns=$(date +%s%N)
read_status=0
cat "${target_file}" > /dev/null 2>&1 || read_status=$?
read_ms=$(( ($(date +%s%N) - start_ns) / 1000000 ))

echo "READ OF UNREADABLE CHUNK: exit ${read_status}, ${read_ms} ms"
cat "${proxy_log}"

# A missing chunk must fail normally without triggering steady-state discovery.
assert_not_equal 0 "${read_status}"

MESSAGE='no on-demand chunk query while no registration is in progress' \
	expect_failure test -f "${query_marker}"
assert_success kill -0 "${proxy_pid}"

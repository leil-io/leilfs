timeout_set '3 minutes'

# A master reporting 5.13.0 may lack pull registration. Drop START/CREDIT and
# close on unsupported END packets to check timeout fallback and forced push.

CHUNKSERVERS=1 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	CHUNKSERVER_EXTRA_CONFIG="MASTER_RECONNECTION_DELAY = 1" \
	setup_local_empty_saunafs info

FILE_SIZE=1M file-generate "${info[mount0]}/file"
saunafs_chunkserver_daemon 0 stop

get_next_port_number proxy_port
ready_marker="${TEMP_DIR}/proxy-ready"
proxy_log="${TEMP_DIR}/no-pull-master-proxy.log"
python3 "${SOURCE_DIR}/tests/tools/registration_no_pull_master_proxy.py" \
	--host "$(get_ip_addr)" --listen-port "${proxy_port}" --master-port "${info[matocs]}" \
	--ready-marker "${ready_marker}" > "${proxy_log}" 2>&1 &
proxy_pid=$!
trap 'kill "${proxy_pid}" 2>/dev/null || true; wait "${proxy_pid}" 2>/dev/null || true' EXIT
assert_eventually 'test -f "${ready_marker}"'

kills() {
	grep -c "END received, killed" "${proxy_log}" || true
}

connections() {
	grep -Ec '^connection [0-9]+ accepted$' "${proxy_log}" || true
}

# Phase 1: default settings with a 1 s start timeout.
add_lines_sfschunkserver_cfg_ "MASTER_HOST = $(get_ip_addr)|MASTER_PORT = ${proxy_port}`
		`|CHUNK_REGISTRATION_START_TIMEOUT = 1" 0
saunafs_chunkserver_daemon 0 start

# Premise: the start timeout fired and the chunkserver reconnected at least once.
MESSAGE='premise: the chunkserver fell back after the start timeout' \
	assert_eventually 'grep -q "connection 2 accepted" "${proxy_log}"' '30 seconds'

# Observe a fixed window of reconnect cycles (each cycle is about 2 s).
deadline=$((SECONDS + 20))
while ((SECONDS < deadline)); do sleep 1; done
phase1_kills=$(kills)
echo "PHASE 1 proxy log:"
cat "${proxy_log}"
MESSAGE="no END to a master that never started pull (kills: ${phase1_kills})" \
	expect_equals 0 "${phase1_kills}"
MESSAGE='timeout fallback must settle on the second connection' \
	expect_equals 2 "$(connections)"
assert_equals 1 "$(count_registered_parts "${info[mount0]}/file")"
assert_success file-validate "${info[mount0]}/file"

# Phase 2: forced push.
saunafs_chunkserver_daemon 0 stop
add_lines_sfschunkserver_cfg_ "CHUNK_REGISTRATION_FORCE_PUSH = 1" 0
before_phase2=$(kills)
before_phase2_connections=$(connections)
saunafs_chunkserver_daemon 0 start

deadline=$((SECONDS + 20))
while ((SECONDS < deadline)); do sleep 1; done
phase2_kills=$(( $(kills) - before_phase2 ))
echo "PHASE 2 proxy log:"
cat "${proxy_log}"
MESSAGE="forced push must not be disconnected each time (kills: ${phase2_kills})" \
	expect_equals 0 "${phase2_kills}"
MESSAGE='forced push must keep its initial connection' \
	expect_equals "$((before_phase2_connections + 1))" "$(connections)"
assert_equals 1 "$(count_registered_parts "${info[mount0]}/file")"
assert_success file-validate "${info[mount0]}/file"
assert_success kill -0 "${proxy_pid}"

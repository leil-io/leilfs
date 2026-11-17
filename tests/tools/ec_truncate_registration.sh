# Shared fixture for EC truncate admission, completion and lock regressions.
# Only the additional empty peer is synthetic; mounts and all three EC parts are real.

ec_truncate_cleanup() {
	local pid
	touch "$TEMP_DIR/prepared-release" "$TEMP_DIR/peer-release"
	for pid in "${ec_truncate_pids[@]}"; do
		kill "$pid" 2>/dev/null || true
		wait "$pid" 2>/dev/null || true
	done
}

ec_truncate_setup() {
	local master_config='USE_CHUNKSERVER_SIDE_CHUNK_LOCK = 0'
	master_config+='|CHUNK_REGISTRATION_CHUNKS_PER_SECOND = 100'
	master_config+='|CHUNK_REGISTRATION_BULK_SIZE = 1|CHUNK_REGISTRATION_WINDOW = 1'
	master_config+="|ON_DEMAND_CHUNK_QUERY_TIMEOUT_MS = ${EC_TRUNCATE_QUERY_TIMEOUT_MS:-1000}"
	master_config+="|ON_DEMAND_CHUNK_QUERY_LIMIT = ${EC_TRUNCATE_QUERY_LIMIT:-10000}"

	CHUNKSERVERS=3 \
		MOUNTS=3 \
		USE_RAMDISK=YES \
		MASTER_CUSTOM_GOALS='10 ec21: $ec(2,1)' \
		MASTER_EXTRA_CONFIG="$master_config" \
		CHUNKSERVER_EXTRA_CONFIG='MASTER_RECONNECTION_DELAY = 1' \
		MOUNT_EXTRA_CONFIG='sfscachemode=NEVER,sfsioretries=1,sfsattrcacheto=0' \
		setup_local_empty_saunafs info

	ec_truncate_pids=()
	trap ec_truncate_cleanup EXIT
	truncate_length=$((300 * 1024))
	target_file="${info[mount0]}/ec21/target"
	verification_file="${info[mount2]}/ec21/target"

	mkdir "${info[mount0]}/ec21"
	assert_success saunafs setgoal ec21 "${info[mount0]}/ec21"
	FILE_SIZE=1M file-generate "$target_file"
	assert_eventually_equals 'echo 3' "count_registered_parts '$target_file'"
	cp "$target_file" "$TEMP_DIR/original"
	head -c "$truncate_length" "$TEMP_DIR/original" > "$TEMP_DIR/truncated"

	# Establish that this topology supports the requested partial EC truncate.
	cp "$target_file" "${info[mount0]}/ec21/control"
	assert_success truncate -s "$truncate_length" "${info[mount0]}/ec21/control"
	assert_files_equal "$TEMP_DIR/truncated" "${info[mount0]}/ec21/control"
}

ec_truncate_trace_mount() {
	local mount_id=$1 prefix=$2
	local proxy_port
	local extra_args=()
	if [[ ${3:-} == hold ]]; then
		extra_args+=(--prepared-release-marker "$TEMP_DIR/prepared-release")
	fi

	saunafs_mount_unmount "$mount_id"
	get_next_port_number proxy_port
	python3 "$SOURCE_DIR/tests/tools/registration_truncate_client_proxy.py" \
		--host "$(get_ip_addr)" --listen-port "$proxy_port" --master-port "${info[matocl]}" \
		--prefix "$prefix" "${extra_args[@]}" > "$prefix.log" 2>&1 &
	ec_truncate_pids+=("$!")
	assert_eventually "test -f '$prefix-ready'"

	echo "sfsport=$proxy_port" >> "${info[mount${mount_id}_cfg]}"
	saunafs_mount_start "$mount_id"
}

ec_truncate_start_peer() {
	local extra_args=()
	local service_port
	if [[ ${1:-} == legacy ]]; then
		extra_args+=(--legacy)
	fi

	get_next_port_number service_port
	python3 "$SOURCE_DIR/tests/tools/registration_incomplete_peer.py" \
		--host "$(get_ip_addr)" --master-port "${info[matocs]}" \
		--service-port "$service_port" --prefix "$TEMP_DIR/peer" \
		"${extra_args[@]}" > "$TEMP_DIR/peer.log" 2>&1 &
	ec_truncate_pids+=("$!")
	assert_eventually 'test -f "$TEMP_DIR/peer-ready"'

	# The incomplete peer owns none of these parts; degraded availability is not involved.
	assert_equals 3 "$(count_registered_parts "$target_file")"
}

ec_truncate_start_operation() {
	(
		result=0
		truncate -s "$truncate_length" "$target_file" > "$TEMP_DIR/truncate.log" 2>&1 \
			|| result=$?
		echo "$result" > "$TEMP_DIR/truncate-result.tmp"
		mv "$TEMP_DIR/truncate-result.tmp" "$TEMP_DIR/truncate-result"
	) &
	truncate_pid=$!
	ec_truncate_pids+=("$truncate_pid")
}

ec_truncate_release_peer() {
	touch "$TEMP_DIR/peer-release"
	assert_eventually 'test -f "$TEMP_DIR/peer-completed"'
}

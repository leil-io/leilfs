MOUNTS=4 \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

# Mounts report their info to the master asynchronously after connecting, so right after
# mounting or after a master restart the admin command can return blocks containing only
# the "Session ID:" header. Poll until every session has a complete mount info block
# instead of sleeping a fixed time, which is not enough on loaded machines.
mount_infos_are_complete() {
	local expected_count=$1
	local mounts
	mounts=$(saunafs_admin_command list-mount-info localhost "${info[matocl]}") || return 1
	[[ $(grep -c "^Session ID:" <<<"${mounts}") -eq ${expected_count} ]] || return 1
	[[ $(grep -c "^SAUNAFS CLIENT MOUNT INFO:" <<<"${mounts}") -eq ${expected_count} ]] || return 1
}

# wait for master server to populate mount info for each mountpoint
assert_eventually 'mount_infos_are_complete 4' "30 seconds"

check_mount_infos() {
	declare -A expected_map
	declare -A actual_map

	# get mount infos from master server using corresponding admin command
	local mounts
	mounts=$(saunafs_admin_command list-mount-info localhost "${info[matocl]}")

	# count sessions
	local session_count
	session_count=$(grep -c "^Session ID:" <<<"$mounts")
	expect_equals "4" "$session_count"

	# ---- Build expected map ----
	# This map will contain the content from each mount info file on
	# each mountpoint.
	for i in $(seq $((session_count-1)) -1 0); do
		local content
		content="$(< "${info[mount$i]}/.saunafs_mount_info")"
		# The file doesn't have Session ID, so add it for comparison
		local session_id=$((i+1))
		expected_map["$session_id"]="Session ID: $session_id"$'\n'"$content"
	done

	# ---- Build actual map ----
	# This map will contain the content from each mount info block
	# returned by the admin command, which directly requests this
	# values from the master server.
	for idx in $(seq 1 "$session_count"); do
		local block
		block=$(awk -v RS='' -v ORS='' "NR==$idx{print; exit}" <<<"$mounts")
		local sid
		sid=$(grep -m1 "^Session ID:" <<<"$block" | awk '{print $3}')
		actual_map["$sid"]="$block"
	done

	# ---- Compare maps ----
	expect_equals "${#expected_map[@]}" "${#actual_map[@]}"

	for sid in "${!expected_map[@]}"; do
		if [[ -z "${actual_map[$sid]}" ]]; then
			echo "Missing Session ID in actual: $sid" >&2
			return 1
		fi
		expect_equals "${expected_map[$sid]}" "${actual_map[$sid]}"
	done
}

check_mount_infos

# check mount infos are the same after master server restart
assert_success saunafs_master_daemon stop
sleep 5
assert_success saunafs_master_daemon start

# wait for the mounts to reconnect and report their info to the restarted master
assert_eventually 'mount_infos_are_complete 4' "30 seconds"

check_mount_infos

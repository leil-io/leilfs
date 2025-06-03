MOUNTS=4 \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

# Pause briefly to give the master server time
# to update the mountpoint info data from each
# active client.
sleep 3

# run the admin command with the list-mount-info option
mounts=$(saunafs_admin_command list-mount-info localhost "${info[matocl]}")

# 1) make sure we got one “Session ID:” header per mount
session_count=$(grep -c "^Session ID:" <<<"$mounts")
expect_equals "4" "$session_count"

# 2) build array of expected mount‐info files
expected_infos=()
for i in $(seq $((session_count-1)) -1 0); do
  expected_infos+=( "$(< "${info[mount$i]}/.saunafs_mount_info")" )
done

# 3) collect each blank‐line paragraph as one “record” with awk
actual_infos=()
for idx in $(seq 1 "$session_count"); do
  # grab the idx-th paragraph (records are separated by empty lines)
  para=$(awk -v RS='' -v ORS='' "NR==$idx{print; exit}" <<<"$mounts")
  # strip off the first line ("Session ID: …")
  mi=${para#*$'\n'}
  actual_infos+=( "$mi" )
done

# 4) compare lengths
expect_equals "${#expected_infos[@]}" "${#actual_infos[@]}"

# 5) compare each block
for i in "${!expected_infos[@]}"; do
  expect_equals "${expected_infos[i]}" "${actual_infos[i]}"
done

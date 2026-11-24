timeout_set 3 minutes

if [[ "${METADATA_BACKEND:-FILE}" != FILE ]]; then test_end; fi

# A source-built reader can also be used on runners without released packages.
reader="${SAUNAFS_5_12_METARESTORE:-}"
if [[ -z "${reader}" ]]; then
	SAUNAFSXX_DIR="${SAUNAFSXX_DIR_BASE}/install/usr"
	SAUNAFSXX_TAG=5.12.0 SAUNAFSXX_TAG_APT=5.12.0-1 install_saunafsXX
	reader="${SAUNAFSXX_DIR}/sbin/sfsmetarestore"
fi
assert_matches 'version: 5[.]12[.]0([^0-9.]|$)' "$("${reader}" -v)"

CHUNKSERVERS=1 \
	MOUNTS=2 \
	AUTO_SHADOW_MASTER=NO \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="enablefilelocks=1|sfscachemode=NEVER" \
	setup_local_empty_saunafs info
source "${SOURCE_DIR}/tests/tools/session_lock_helpers.sh"

# Capture a checkpoint with one exclusive holder and another session waiting for the lock.
assert_success touch "${info[mount0]}/contended"
inode=$(stat -c %i "${info[mount0]}/contended")
lock_kinds[contended]=flock
session_lock_start_process contended 0 w holder
assert_eventually_prints 1 "session_lock_count ${info[matocl]} flock active ${inode}"
session_lock_start_process contended 1 w waiter
assert_eventually_prints 1 "session_lock_count ${info[matocl]} flock pending ${inode}"
holder=$(session_lock_rows "${info[matocl]}" flock active "${inode}" | awk '{print $3}')
waiter=$(session_lock_rows "${info[matocl]}" flock pending "${inode}" | awk '{print $3}')
data_dir="${info[master0_data_path]}"
session_lock_dump 0 "${TEMP_DIR}/original-locks"
assert_success cp "${data_dir}/metadata.sfs" "${TEMP_DIR}/checkpoint"

reply=$(leil-admin delete-session localhost "${info[matocl]}" "${holder}")
assert_equals "Session deleted successfully" "${reply}"
assert_eventually "grep -q '^write lock:' '${TEMP_DIR}/contended-waiter.log'"
session_lock_dump 0 "${TEMP_DIR}/final-locks"
get_changes "${data_dir}" > "${TEMP_DIR}/journal"

# Stop immediately after the marker. The old reader must consume its version and
# preserve both the active and pending locks until the real unlock arrives.
assert_success awk -F'|' -v marker="CLRLCK(0,0,${holder})" '
	{print}
	$2 == marker {found=1; exit}
	END {exit !found}
' "${TEMP_DIR}/journal" > "${TEMP_DIR}/through-marker"
marker_version=$(tail -1 "${TEMP_DIR}/through-marker" | cut -d: -f1)
assert_success "${reader}" -m "${TEMP_DIR}/checkpoint" -o "${TEMP_DIR}/at-marker.sfs" \
	"${TEMP_DIR}/through-marker"
session_lock_image "${TEMP_DIR}/at-marker.sfs" "${TEMP_DIR}/at-marker-locks"
restored_version=$(awk '/^# maxnodeid:/ {print $6}' "${TEMP_DIR}/at-marker-locks.dump")
assert_equals "$((marker_version + 1))" "${restored_version}"
assert_files_equal "${TEMP_DIR}/original-locks" "${TEMP_DIR}/at-marker-locks"

# Continue from the old reader's own checkpoint through the grant to the waiter.
assert_success "${reader}" -m "${TEMP_DIR}/at-marker.sfs" -o "${TEMP_DIR}/restored.sfs" \
	"${TEMP_DIR}/journal"
session_lock_image "${TEMP_DIR}/restored.sfs" "${TEMP_DIR}/restored-locks"
assert_files_equal "${TEMP_DIR}/final-locks" "${TEMP_DIR}/restored-locks"
assert_equals "${waiter}" "$(awk -v inode="${inode}" \
	'$1 == "flock" && $2 == "active" && $3 == inode {print $6}' "${TEMP_DIR}/restored-locks")"

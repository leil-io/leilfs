timeout_set 5 minutes

# The upgrade harness extracts the released reader from the same master package as the daemon.
legacy_version=5.12.0
sustain=$(timeout_rescale_seconds 60)
CHUNKSERVERS=1 \
	MOUNTS=4 \
	AUTO_SHADOW_MASTER=NO \
	USE_RAMDISK=YES \
	START_WITH_LEGACY_SAUNAFS=YES \
	SAUNAFSXX_TAG="${legacy_version}" \
	SAUNAFSXX_TAG_APT="${legacy_version}-1" \
	MASTER_EXTRA_CONFIG="SESSION_SUSTAIN_TIME=${sustain}" \
	MOUNT_EXTRA_CONFIG="enablefilelocks=1|sfscachemode=NEVER" \
	setup_local_empty_saunafs info

reader="${SAUNAFSXX_DIR}/sbin/sfsmetarestore"
assert_success test -x "${reader}"
assert_awk_finds "/${legacy_version//./\\.}/" "$("${reader}" -v)"
assert_success test "${SAUNAFS_VERSION}" != "${legacy_version}"
assert_equals 1 "$(saunafs_old_admin_master info | grep -c "v${legacy_version//./\\.}$")"

# Start from a released checkpoint, so only the new journal records exercise the old reader.
for mount in 0 1 2 3; do saunafs_mount_unmount "${mount}"; done
assert_success saunafsXX_master_daemon stop
assert_success cp "${info[master0_data_path]}/metadata.sfs" "${TEMP_DIR}/released-checkpoint"
assert_success saunafs_master_daemon start
for mount in 0 1 2 3; do saunafs_mount_start "${mount}"; done
source "${SOURCE_DIR}/tests/tools/session_lock_helpers.sh"
session_lock_prepare
# Exclude earlier releases during fixture setup; the prefix must end within session cleanup.
cleanup_boundary=$(get_changes "${info[master0_data_path]}" | tail -1 | cut -d: -f1)
assert_matches '^[0-9]+$' "${cleanup_boundary}"
session_lock_release_holders
session_lock_check_release
session_lock_dump 0 "${TEMP_DIR}/expected-locks"

# Preserve the journal before stopping clients can generate further owner releases.
assert_success saunafs_master_daemon stop
get_changes "${info[master0_data_path]}" > "${TEMP_DIR}/cleanup-changelog"
assert_success "${reader}" -m "${TEMP_DIR}/released-checkpoint" \
	-o "${TEMP_DIR}/restored.sfs" "${TEMP_DIR}/cleanup-changelog" \
	> "${TEMP_DIR}/released-reader.log" 2>&1
assert_success python3 "${SOURCE_DIR}/tests/tools/session_lock_image.py" \
	"${TEMP_DIR}/restored.sfs" > "${TEMP_DIR}/restored-locks"
assert_files_equal "${TEMP_DIR}/expected-locks" "${TEMP_DIR}/restored-locks"

# Replay a crash just after orphan purge, before later cleanup can hide a stranded lock.
orphan_inode="${lock_inodes[orphan-flock]}"
assert_success awk -F: -v boundary="${cleanup_boundary}" -v inode="${orphan_inode}" '
	{print}
	$1 > boundary && index($0, "|RELEASE(" inode ",") {found=1; exit}
	END {exit !found}
' "${TEMP_DIR}/cleanup-changelog" > "${TEMP_DIR}/cleanup-prefix"
assert_success "${reader}" -m "${TEMP_DIR}/released-checkpoint" \
	-o "${TEMP_DIR}/prefix-restored.sfs" "${TEMP_DIR}/cleanup-prefix" \
	> "${TEMP_DIR}/prefix-reader.log" 2>&1
assert_success python3 "${SOURCE_DIR}/tests/tools/session_lock_image.py" \
	"${TEMP_DIR}/prefix-restored.sfs" > "${TEMP_DIR}/prefix-locks"
assert_equals 0 "$(awk -v inode="${orphan_inode}" \
	'$3 == inode {count++} END {print count+0}' "${TEMP_DIR}/prefix-locks")"
cleanup_session_locks

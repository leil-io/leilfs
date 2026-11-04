timeout_set 5 minutes

# A released Master's historical cleanup must keep its meaning in this build's Shadow.
legacy_version=5.12.0
sustain=$(timeout_rescale_seconds 60)
CHUNKSERVERS=1 \
	MASTERSERVERS=2 \
	MOUNTS=4 \
	USE_RAMDISK=YES \
	START_WITH_LEGACY_SAUNAFS=YES \
	SAUNAFSXX_TAG="${legacy_version}" \
	SAUNAFSXX_TAG_APT="${legacy_version}-1" \
	MASTER_EXTRA_CONFIG="SESSION_SUSTAIN_TIME=${sustain}" \
	MOUNT_EXTRA_CONFIG="enablefilelocks=1|sfscachemode=NEVER" \
	setup_local_empty_saunafs info

assert_success test "${SAUNAFS_VERSION}" != "${legacy_version}"
assert_equals 1 "$(saunafs_old_admin_master info | grep -c "v${legacy_version//./\\.}$")"
for mount in 0 1 2 3; do
	saunafs_mount_unmount "${mount}"
	saunafs_mount_start "${mount}"
done
assert_success saunafs_master_n 1 start
source "${SOURCE_DIR}/tests/tools/session_lock_helpers.sh"
session_lock_prepare
session_lock_shadow_matches
session_lock_release_holders

# The released bug answers both waiters but installs neither; replay must preserve that state.
for name in "${lock_cases[@]}"; do
	assert_eventually_prints 2 "session_lock_grants ${name}"
	session_lock_assert_state "${name}" 0 0
	assert_failure grep -Eq 'failed:|Failed to' \
		"${lock_logs[${name},2]}" "${lock_logs[${name},3]}"
done
session_lock_shadow_matches

# Keep waiter FDs open through promotion so their closes cannot mask an extra replayed holder.
saunafs_master_daemon kill
saunafs_make_conf_for_shadow 0
saunafs_make_conf_for_master 1
saunafs_master_daemon reload
assert_eventually "leil-admin info localhost ${info[matocl]} >/dev/null"
session_lock_dump 1 "${TEMP_DIR}/promoted-locks"
assert_files_equal "${TEMP_DIR}/master-locks" "${TEMP_DIR}/promoted-locks"
cleanup_session_locks

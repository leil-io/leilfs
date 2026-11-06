timeout_set 5 minutes

# Start from a released store, then move a current mount from the current daemon back to it.
legacy_version=5.12.0
CHUNKSERVERS=1 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	START_WITH_LEGACY_SAUNAFS=YES \
	SAUNAFSXX_TAG="${legacy_version}" \
	SAUNAFSXX_TAG_APT="${legacy_version}-1" \
	MASTER_EXTRA_CONFIG="AUTO_RECOVERY=1" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER|sfsattrcacheto=0|sfsdirentrycacheto=0" \
	setup_local_empty_saunafs info

assert_success saunafs_mount_unmount 0
assert_success saunafsXX_master_daemon stop
assert_success saunafs_master_daemon start
assert_success saunafs_mount_start 0
saunafs_wait_for_all_ready_chunkservers

session_ids() {
	saunafs_admin_master list-sessions | awk '/^Session ID:/ {print $3}'
}
assert_eventually_prints 1 'session_ids | wc -l' "1 minute"
session=$(session_ids)
assert_matches '^[0-9]+$' "${session}"

echo before > "${info[mount0]}/file"
exec 3>>"${info[mount0]}/file"
assert_success saunafs_master_daemon stop
assert_success saunafsXX_master_daemon start
assert_equals 1 "$(saunafs_old_admin_master info | grep -c "v${legacy_version//./\\.}$")"
saunafs_wait_for_all_ready_chunkservers

assert_success touch "${info[mount0]}/after"
echo after >&3
exec 3>&-
assert_equals "$(printf 'before\nafter')" "$(cat "${info[mount0]}/file")"
assert_equals "${session}" "$(session_ids)"

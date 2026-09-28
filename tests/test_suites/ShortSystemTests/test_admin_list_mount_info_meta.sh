# A meta mount has no data path, so it fills its session fields on its own before it reports.
# The master must list complete mount info for it, like for an ordinary mount.
timeout_set 150 seconds

MOUNTS=2 \
	MOUNT_1_EXTRA_CONFIG="sfsmeta" \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

# The meta mount's block is the one whose options carry sfsmeta
meta_block() {
	saunafs_admin_command list-mount-info localhost "${info[matocl]}" | awk -v RS='' '/sfsmeta: 1/'
}

assert_eventually_prints 1 'meta_block | grep -c "^SAUNAFS CLIENT MOUNT INFO:"' "30 seconds"
block=$(meta_block)
assert_awk_finds "/^USERNAME: $(id -un)\$/" "${block}"
assert_awk_finds '/^PID: [1-9][0-9]*$/' "${block}"
assert_awk_finds "/^$(grep '^VERSION:' "${info[mount0]}/.saunafs_mount_info")\$/" "${block}"

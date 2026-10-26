timeout_set 6 minutes

# Master, Shadow and chunkservers of this build must work with those of the last release before
# the versioned chunk lock, which know only today's lock, in every mix an upgrade goes through:
# released Master and Shadow with chunkservers of this build; a released Master followed by this
# build's Shadow, which is then promoted; this build's Master and Shadow with released
# chunkservers. A Master refuses a Shadow older than itself, so a released Shadow under this
# build's Master is not one of them. In each phase, writes, overwrites and truncates of replicated
# and erasure-coded files succeed and every file written so far reads back. Chunkserver locks are
# on, as the test setup does by default.
legacy_version="5.12.0"

CHUNKSERVERS=3 \
	MASTERSERVERS=2 \
	USE_RAMDISK=YES \
	START_WITH_LEGACY_SAUNAFS=YES \
	SAUNAFSXX_TAG="${legacy_version}" \
	SAUNAFSXX_TAG_APT="${legacy_version}-1" \
	MASTER_CUSTOM_GOALS="10 ec21: \$ec(2,1)" \
	MASTER_EXTRA_CONFIG="OPERATIONS_DELAY_INIT = 0|OPERATIONS_DELAY_DISCONNECT = 0" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

# The cluster really starts on the released version; the mixes below would be vacuous otherwise.
assert_success test "${SAUNAFS_VERSION}" != "${legacy_version}"
assert_equals 1 $(saunafs_old_admin_master info | grep -c "v${legacy_version//./\\.}$")

# The client is this build's: the client protocol is not what this test is about.
saunafs_mount_unmount 0
saunafs_mount_start 0
mkdir "${info[mount0]}/replicated" "${info[mount0]}/erasure"
leil setgoal 2 "${info[mount0]}/replicated"
leil setgoal ec21 "${info[mount0]}/erasure"
mkdir "${TEMP_DIR}/expected"

# Writes a file in both directories, overwrites a block of it and truncates it, keeping the
# expected contents; then every file written so far must read back.
workload() {
	local name=${1}
	local dir
	for dir in replicated erasure; do
		local file="${info[mount0]}/${dir}/${name}"
		local expected="${TEMP_DIR}/expected/${dir}_${name}"
		head -c 3M /dev/urandom > "${expected}"
		assert_success cp "${expected}" "${file}"
		head -c 64K /dev/urandom > "${TEMP_DIR}/block"
		assert_success dd if="${TEMP_DIR}/block" of="${file}" bs=64K seek=5 count=1 \
			conv=notrunc,fsync status=none
		dd if="${TEMP_DIR}/block" of="${expected}" bs=64K seek=5 count=1 conv=notrunc \
			status=none
		assert_success truncate -s 2M "${file}"
		truncate -s 2M "${expected}"
	done
	local each
	for each in "${TEMP_DIR}"/expected/*; do
		local base=${each##*/}
		assert_success cmp "${each}" "${info[mount0]}/${base%%_*}/${base#*_}"
	done
}

# Promotes shadow ${1} to master once it is synchronized; the old master ${2} is killed.
promote() {
	assert_eventually "saunafs_shadow_synchronized ${1}"
	saunafs_master_daemon kill
	saunafs_make_conf_for_shadow "${2}"
	saunafs_make_conf_for_master "${1}"
	saunafs_master_daemon reload
	saunafs_wait_for_all_ready_chunkservers
}

# Released Master and Shadow, chunkservers of this build.
saunafsXX_shadow_daemon_n 1 start
for cs in 0 1 2; do
	saunafsXX_chunkserver_daemon "${cs}" stop
	assert_success saunafs_chunkserver_daemon "${cs}" start
done
saunafs_wait_for_all_ready_chunkservers
workload released_master

# Released Master, this build's Shadow; the Shadow is promoted and serves everything.
saunafs_master_n 1 restart
workload new_shadow
promote 1 0
workload new_master_after_promotion

# This build's Master and Shadow, released chunkservers.
saunafs_master_n 0 start
assert_eventually "saunafs_shadow_synchronized 0"
for cs in 0 1 2; do
	saunafs_chunkserver_daemon "${cs}" stop
	assert_success saunafsXX_chunkserver_daemon "${cs}" start
done
saunafs_wait_for_all_ready_chunkservers
workload released_chunkservers

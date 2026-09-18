timeout_set 2 minutes

# Shadow synchronization against the forkless (FoundationDB) metadata backend.
#
# A forkless shadow never downloads a metadata image: it reads the live FDB keyspace, rolls each
# section back to the last sealed checkpoint, and then converges by replaying the changelog it
# downloads from the master. This test drives a broad set of metadata mutations through that path
# and proves the shadow tracks the master both from a sealed checkpoint and across later changes.

# METADATA_DUMP_PERIOD_SECONDS = 0 disables periodic checkpoint sealing, so the checkpoint the
# shadow loads is the one sealed explicitly below rather than an arbitrary later one.
master_cfg="METADATA_DUMP_PERIOD_SECONDS = 0"
master_cfg+="|OPERATIONS_DELAY_INIT = 1"
master_cfg+="|CHUNKS_LOOP_MIN_TIME = 1|CHUNKS_LOOP_MAX_CPU = 90"
master_cfg+="|MAGIC_DEBUG_LOG = ${TEMP_DIR}/master0.log|LOG_FLUSH_ON=DEBUG"

CHUNKSERVERS=3 \
	METADATA_BACKEND="FORKLESS" \
	MASTERSERVERS=2 \
	MOUNTS=2 \
	USE_RAMDISK="YES" \
	MOUNT_0_EXTRA_CONFIG="sfscachemode=NEVER,sfsreportreservedperiod=1,sfsdirentrycacheto=0" \
	MOUNT_1_EXTRA_CONFIG="sfsmeta" \
	SFSEXPORTS_EXTRA_OPTIONS="allcanchangequota,ignoregid" \
	SFSEXPORTS_META_EXTRA_OPTIONS="nonrootmeta" \
	MASTER_0_EXTRA_CONFIG="$master_cfg" \
	setup_local_empty_saunafs info

# The meta mount backs metadata_generate_trash_ops; the changelog path lets the generators verify
# the changes they produced.
export SFS_META_MOUNT_PATH=${info[mount1]}
export CHANGELOG="${info[master0_data_path]}"/changelog.sfs

# Generate a lot of different changes
cd "${info[mount0]}"
metadata_generate_all
cd

# Seal a checkpoint in FDB: fs_storeall() drains the writer queue and publishes a new checkpoint
# version.
assert_success saunafs_admin_master save-metadata
assert_file_exists "${TEMP_DIR}/master0.log"
assert_awk_finds '/Checkpoint version [0-9]+ sealed successfully/' "$(cat "${TEMP_DIR}/master0.log")"

# Verify the shadow can rebuild its image from the sealed checkpoint and converge by replaying the
# changelog.
saunafs_master_n 1 start
assert_eventually 'saunafs_shadow_synchronized 1'

# Verify if we can modify the filesystem and all the changes would be applied by the shadow master
cd "${info[mount0]}"
rm -rf * || true
assert_success grep -q CHECKSUM "$CHANGELOG"
assert_eventually 'saunafs_shadow_synchronized 1'

# A shadow that fails to apply the changelog asks the master to store metadata
# (SAU_MLTOMA_CHANGELOG_APPLY_ERROR), which seals a fresh checkpoint the shadow can then load
# wholesale. That would let a broken rollback or replay path converge from already-current state,
# so the assertions above would pass without exercising it.
log=$(cat "${TEMP_DIR}/master0.log")
assert_awk_finds_no '/SAU_MLTOMA_CHANGELOG_APPLY_ERROR/' "$log"

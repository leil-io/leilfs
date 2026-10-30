timeout_set 30 seconds
master_cfg+="MAGIC_DEBUG_LOG = $TEMP_DIR/syslog|LOG_FLUSH_ON=DEBUG"
touch "$TEMP_DIR/syslog"

CHUNKSERVERS=0 \
	MASTERSERVERS=2 \
	USE_RAMDISK="YES" \
	MASTER_EXTRA_CONFIG="${master_cfg}" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	AUTO_SHADOW_MASTER="YES" \
	setup_local_empty_saunafs info

metadata_file="${info[master0_data_path]}/metadata.leil"

ACTUAL_HEADER_TAG="SFSM 2.9"
ACTUAL_TRAILER_TAG="[SFS EOF MARKER]"
LEGACY_HEADER_TAG="LIZM 2.9"
LEGACY_TRAILER_TAG="[MFS EOF MARKER]"

# Add metalogger service to the configuration
saunafs_metalogger_daemon start

# Access mountpoint to create metadata
cd "${info[mount0]}"
for i in {1..100}; do
	touch file$i
	mkdir dir$i
done

# Exit mount point and save metadata by stopping master server
cd "${TEMP_DIR}"
assert_success saunafs_master_n 0 stop
assert_success saunafs_master_n 1 stop

# Expected Values
assert_equals "$(head -c8 ${metadata_file})" "${ACTUAL_HEADER_TAG}"
assert_equals "$(tail -c16 ${metadata_file})" "${ACTUAL_TRAILER_TAG}"

# Modify metadata signature for legacy and reload metadata by starting master
echo -n "LIZ" | dd of=${metadata_file} bs=1 seek=0 count=3 conv=notrunc
echo -n "MFS" | dd of=${metadata_file} bs=1 seek=$(( $(stat --print="%s" ${metadata_file}) - 15)) count=3 conv=notrunc
assert_equals "$(head -c8 ${metadata_file})" "${LEGACY_HEADER_TAG}"
assert_equals "$(tail -c16 ${metadata_file})" "${LEGACY_TRAILER_TAG}"

mv -iv "${info[master0_data_path]}/metadata.leil" "${info[master0_data_path]}/metadata.mfs"
assertlocal_file_exists "${info[master0_data_path]}/metadata.mfs"

# Start master server to reload metadata legacy format
assertlocal_success saunafs_master_n 0 start
# Restart master server to load metadata in new format when both are present
assertlocal_success saunafs_master_n 0 restart
assertlocal_file_exists "${info[master0_data_path]}/metadata.leil"
assertlocal_file_exists "${info[master0_data_path]}/metadata.mfs"

# Shadows has not  synchronize metadata at this point
assertlocal_file_not_exists "${info[master1_data_path]}/metadata.leil"
#start shadow server to synchronize metadata
assert_success saunafs_master_n 1 start
assert_eventually "saunafs_shadow_synchronized 1"
assertlocal_file_exists "${info[master1_data_path]}/metadata.leil"

# Stop master server to save metadata in new format
assert_success saunafs_master_n 0 stop

# Synchronized metadata in shadows has proper Tags
SHADOW_METADATA_FILE="${info[master1_data_path]}/metadata.leil"
assert_equals "$(head -c8 ${SHADOW_METADATA_FILE})" "${ACTUAL_HEADER_TAG}"
assert_equals "$(tail -c16 ${SHADOW_METADATA_FILE})" "${ACTUAL_TRAILER_TAG}"

# Stop metalogger service and verify created changelogs
saunafs_metalogger_daemon stop
assertlocal_file_exists "${info[master0_data_path]}/changelog_ml.leil"

# Expected Values after reload
metadata_file="${info[master0_data_path]}/metadata.leil"
assert_equals "$(head -c8 ${metadata_file})" "${ACTUAL_HEADER_TAG}"
assert_equals "$(tail -c16 ${metadata_file})" "${ACTUAL_TRAILER_TAG}"

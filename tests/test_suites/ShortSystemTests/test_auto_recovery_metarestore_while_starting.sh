CHUNKSERVERS=0 \
	USE_RAMDISK=YES \
	MASTER_EXTRA_CONFIG="AUTO_RECOVERY = 1" \
	MOUNTS=0 \
	setup_local_empty_saunafs info

# Generate empty metadata file by stopping the master server and generate a long changelog.
saunafs_master_daemon stop
assert_equals 1 $(metadata_get_version "${info[master_data_path]}/metadata.sfs")

# Construct the new-style .LIVE changelog filename
# generate_changelog starts with ID 1
FIRST_ID_VAL=1
FIRST_ID_HEX=$(printf "%016X" "$FIRST_ID_VAL")
# Assume info[cluster_id] and info[hostname] are provided by test setup
# If not, use defaults or `hostname -s`
CLUSTER_ID=${info[cluster_id]:-testcluster}
HOSTNAME=${info[hostname]:-$(hostname -s)}
BEGIN_EPOCH_UTC=$(date -u -d@"$FIRST_ID_VAL" "+%Y-%m-%dT%H%MZ" 2>/dev/null || echo "1970-01-01T0000Z") # Fallback for date command failure

CHANGELOG_FILENAME="changelog.sfs.${CLUSTER_ID}.${FIRST_ID_HEX}.UNDEF.${BEGIN_EPOCH_UTC}.LIVE.${HOSTNAME}"
FULL_CHANGELOG_PATH="${info[master_data_path]}/${CHANGELOG_FILENAME}"

echo "Writing generated changelog to: ${FULL_CHANGELOG_PATH}"
generate_changelog > "${FULL_CHANGELOG_PATH}"

# Start the master server in background and wait until master starts to apply
# the changelog. This process will then last for a couple of seconds.
saunafs_master_daemon start &
assert_eventually 'test -e "${info[master_data_path]}/metadata.sfs.lock"'
sleep 1

# Try restoring metadata. This should fail, because master holds the lock.
expect_failure sfsmetarestore -a -d "${info[master_data_path]}"

# Expect that the master server still applies the changelog.
expect_failure saunafs-admin info localhost "${info[matocl]}"

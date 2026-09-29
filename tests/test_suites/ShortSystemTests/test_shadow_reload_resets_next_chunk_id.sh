timeout_set '2 minutes'

master_cfg="MASTER_TIMEOUT = 10"
# Expire retained changelogs so the reconnect must download an image.
master_cfg+="|MATOML_LOG_PRESERVE_SECONDS = 0"
master_cfg+="|METADATA_DUMP_PERIOD_SECONDS = 0"

CHUNKSERVERS=1 \
	MASTERSERVERS=2 \
	USE_RAMDISK="YES" \
	MASTER_EXTRA_CONFIG="${master_cfg}" \
	setup_local_empty_saunafs info

# Keep an image whose next chunk ID predates the chunks the shadow will later apply.
assert_success saunafs_admin_master save-metadata
saunafs_master_n 1 start
assert_eventually 'saunafs_shadow_synchronized 1'

cd "${info[mount0]}"
FILE_SIZE=1K file-generate chunk{1..5}
cd
assert_eventually 'saunafs_shadow_synchronized 1'
assert_success grep -q CHECKSUM "${info[master0_data_path]}/changelog.sfs"
cp "${info[master1_data_path]}/metadata.sfs" "${TEMP_DIR}/metadata_before_reload.sfs"
syslog_start_line=$(wc -l < /var/log/syslog)
syslog_next_line=$((${syslog_start_line} + 1))

# A reconnect with different metadata versions makes the shadow discard its in-memory image.
assert_eventually_prints 2 \
	'saunafs_admin_master_no_password list-metadataservers --porcelain | wc -l'
shadow_pid=$(saunafs_master_n 1 test | sed 's/.*: //')
assert_matches '^[0-9]+$' "${shadow_pid}"
kill -STOP "${shadow_pid}"
# Wait until the master drops the stopped shadow's connection.
assert_eventually_prints 1 \
	'saunafs_admin_master_no_password list-metadataservers --porcelain | wc -l' \
	'30 seconds'
touch "${info[mount0]}/after_disconnect"
kill -CONT "${shadow_pid}"

# The downloaded image has a lower next chunk ID, but its changelogs must still apply.
assert_eventually "tail -n +${syslog_next_line} /var/log/syslog | \
	grep -q 'unloading filesystem at'"
assert_eventually 'saunafs_shadow_synchronized 1'
assert_failure cmp -s "${TEMP_DIR}/metadata_before_reload.sfs" \
	"${info[master1_data_path]}/metadata.sfs"
reload_log=$(tail -n +"${syslog_next_line}" /var/log/syslog)
assert_awk_finds_no '/Failed to set next chunk ID from metadata file/' "${reload_log}"
assert_awk_finds_no '/Metadata checksum not matching/' "${reload_log}"
FILE_SIZE=1K file-generate "${info[mount0]}/after_reload"
assert_eventually 'saunafs_shadow_synchronized 1'

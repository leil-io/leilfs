CHUNKSERVERS=3 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	MASTER_EXTRA_CONFIG="SFSMETARESTORE_PATH = $TEMP_DIR/restore.sh|MAGIC_PREFER_BACKGROUND_DUMP = 1" \
	setup_local_empty_saunafs info

cat > $TEMP_DIR/restore.sh << END
#!/usr/bin/env bash
touch $TEMP_DIR/dump_started
sleep 5
sfsmetarestore "\$@"
END

chmod +x $TEMP_DIR/restore.sh

touch "${info[mount0]}"/file

# begin dumping
assert_success saunafs_admin_master save-metadata

# before dumping ends, stop the server - it should succeed
saunafs_master_daemon stop

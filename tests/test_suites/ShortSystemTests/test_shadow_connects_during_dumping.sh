master_cfg="SFSMETARESTORE_PATH = $TEMP_DIR/metarestore.sh"
master_cfg+="|MAGIC_PREFER_BACKGROUND_DUMP = 1"
master_cfg+="|METADATA_DUMP_PERIOD_SECONDS = 0"

CHUNKSERVERS=1 \
	MASTERSERVERS=2 \
	USE_RAMDISK="YES" \
	MASTER_EXTRA_CONFIG="$master_cfg" \
	setup_local_empty_saunafs info

# Instead of real sfsmetarestore, provide a program which hangs forever to slow down metadata dumps
cat > "$TEMP_DIR/metarestore.sh" << END
#!/usr/bin/env bash
touch "$TEMP_DIR/dump_started"
sleep 30 # Long enough to do the test, short enough to be able to terminate memchek within 60 s
touch "$TEMP_DIR/dump_finishing"
END
chmod +x "$TEMP_DIR/metarestore.sh"

# Generate some files
cd "${info[mount0]}"
touch file{1..20}
# Start dumping metadata
saunafs_admin_master save-metadata &
touch file{30..40}
saunafs_master_n 1 start                          # Connect shadow master during the dump
assert_eventually "saunafs_shadow_synchronized 1"
touch file{50..60}

# Expect them to synchronize despite of the dump in progress
assert_eventually "saunafs_shadow_synchronized 1"
assert_file_not_exists "$TEMP_DIR/dump_finishing"

# Check if new changes are also being synchronised between metadata servers
rm file{50..60}
assert_eventually "saunafs_shadow_synchronized 1"

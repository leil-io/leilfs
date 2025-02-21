CHUNKSERVERS=6 \
	USE_RAMDISK=YES \
	MASTER_CUSTOM_GOALS="1 ec42: \$ec(4,2)" \
	AUTO_SHADOW_MASTER="NO" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	CHUNKSERVER_EXTRA_CONFIG="GARBAGE_COLLECTION_FREQ_MS = 0`
			`|HDD_TEST_FREQ = 100000`
			`|MAGIC_DEBUG_LOG = $TEMP_DIR/log`
			`|LOG_FLUSH_ON=DEBUG" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

for i in {1..100}; do
	dd if=/dev/zero of=file${i} bs=33 count=1 oflag=direct status=progress
done

number_of_extra_reads=$(grep "hddRead" $TEMP_DIR/log | wc -l);

assert_equals "0" "${number_of_extra_reads}"

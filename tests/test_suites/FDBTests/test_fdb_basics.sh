CHUNKSERVERS=1 \
	METADATA_BACKEND=FDB \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	CHUNKSERVER_EXTRA_CONFIG="GARBAGE_COLLECTION_FREQ_MS = 0|HDD_TEST_FREQ = 100000" \
	AUTO_SHADOW_MASTER="NO" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

assert_success sfs-test-fdb "/tmp/saunafs-fdb-test/conf/fdb.cluster"

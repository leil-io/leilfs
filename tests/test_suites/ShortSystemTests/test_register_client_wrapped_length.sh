CHUNKSERVERS=1 \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

assert_success send_wrapped_length_client_register localhost "${info[matocl]}"

sleep 2

assert_success saunafs_master_daemon isalive

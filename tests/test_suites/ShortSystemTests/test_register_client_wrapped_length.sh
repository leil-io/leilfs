CHUNKSERVERS=0 \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

assert_success send_wrapped_length_client_register localhost "${info[matocl]}" session

assert_success saunafs_master_daemon isalive

assert_success send_wrapped_length_client_register localhost "${info[matocl]}" metasession

assert_success saunafs_master_daemon isalive

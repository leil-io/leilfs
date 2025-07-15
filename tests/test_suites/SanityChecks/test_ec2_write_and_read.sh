timeout_set 70 seconds

CHUNKSERVERS=23 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	CHUNKSERVER_EXTRA_CONFIG="READ_AHEAD_KB = 1024|MAX_READ_BEHIND_KB = 2048" \
	MASTER_CUSTOM_GOALS="8 ec417: \$ec(4,17)"
	setup_local_empty_saunafs info

cd ${info[mount0]}

mkdir dir_ec21
saunafs setgoal -r ec21 dir_ec21
FILE_SIZE=123456789 BLOCK_SIZE=12345 file-generate dir_ec21/file
if ! file-validate dir_ec21/file; then
	test_add_failure "Data read from file is different than written"
fi

mkdir dir_ec32
saunafs setgoal -r ec32 dir_ec32
FILE_SIZE=123456789 BLOCK_SIZE=12345 file-generate dir_ec32/file
if ! file-validate dir_ec32/file; then
	test_add_failure "Data read from file is different than written"
fi

mkdir dir_ec43
saunafs setgoal -r ec43 dir_ec43
FILE_SIZE=123456789 BLOCK_SIZE=12345 file-generate dir_ec43/file
if ! file-validate dir_ec43/file; then
	test_add_failure "Data read from file is different than written"
fi

mkdir dir_turbo_ec
saunafs setgoal -r ec417 dir_turbo_ec
FILE_SIZE=123456789 BLOCK_SIZE=12345 file-generate dir_turbo_ec/file
if ! file-validate dir_turbo_ec/file; then
	test_add_failure "Data read from file is different than written"
fi

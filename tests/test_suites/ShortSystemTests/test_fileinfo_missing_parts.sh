timeout_set 70 seconds

CHUNKSERVERS=7 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER"
	setup_local_empty_saunafs info

cd ${info[mount0]}

mkdir dir_ec
saunafs setgoal -r ec43 dir_ec
FILE_SIZE=123456789 BLOCK_SIZE=12345 file-generate dir_ec/file

saunafs_chunkserver_daemon 0 stop
saunafs_chunkserver_daemon 1 stop
saunafs_chunkserver_daemon 2 stop

assert_failure "saunafs fileinfo dir_ec/file | grep 'not enough parts available'"

saunafs_chunkserver_daemon 3 stop

saunafs fileinfo dir_ec/file | grep "not enough parts available"

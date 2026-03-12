timeout_set 30 seconds

# Let's use a very high write window size to make sure all operations are sent to the chunkservers
# immediately, while also disable write buffering on the chunkservers.

CHUNKSERVERS=8 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER,sfswritewindowsize=384" \
	CHUNKSERVER_EXTRA_CONFIG="WRITE_BUFFERING_SIZE_MB=0" \
	MASTER_CUSTOM_GOALS="8 ec62: \$ec(6,2)"
	setup_local_empty_saunafs info

cd ${info[mount0]}

mkdir dir_ec
saunafs setgoal -r ec62 dir_ec
cd dir_ec

assert_success fio --name=fio_randwrite_test --size=384K --rw=randwrite --numjobs=1 --ioengine=libaio \
	--group_reporting --bs=16K --verify=crc32

assert_success fio --name=fio_randwrite_test --size=384K --rw=randwrite --numjobs=1 --ioengine=libaio \
	--group_reporting --bs=1K --verify=md5 --verify_pattern=%o --do_verify=0

saunafs_chunkserver_daemon 0 stop
saunafs_chunkserver_daemon 1 stop

assert_success fio --name=fio_randwrite_test --size=384K --rw=randwrite --numjobs=1 --ioengine=libaio \
	--group_reporting --bs=1K --verify=md5 --verify_pattern=%o --verify_only

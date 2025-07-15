timeout_set 1 minute

CHUNKSERVERS=4 \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

pseudorandom_init

cd "${info[mount0]}"
mkdir dir
saunafs setgoal ec22 dir
cd dir

for i in {0..19} ; do
	filesize=$( pseudorandom 8 $((6 * SAUNAFS_BLOCK_SIZE)) )
	head -c $filesize </dev/urandom >file${i}_$filesize
done

saunafs_chunkserver_daemon 0 stop
saunafs_chunkserver_daemon 1 stop

for file in * ; do
	MESSAGE="Overwriting $file" expect_success file-overwrite $file
	MESSAGE="Validating overwritten file" expect_success file-validate $file
done

saunafs_chunkserver_daemon 0 start
saunafs_chunkserver_daemon 1 start

saunafs_wait_for_all_ready_chunkservers

for file in * ; do
	MESSAGE="Validating $file after restart" expect_success file-validate $file
done

timeout_set 10 minutes

CHUNKSERVERS=10 \
	DISK_PER_CHUNKSERVER=1 \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	MASTER_CUSTOM_GOALS="10 ec_9_1: \$ec(9,1)" \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

dir="${info[mount0]}/dir"
mkdir "$dir"
saunafs setgoal ec_9_1 "$dir"
FILE_SIZE=876M file-generate "$dir/file"

for i in {0..9}; do
	saunafs_chunkserver_daemon $i stop
	if ! file-validate "$dir/file"; then
		test_add_failure "Data read from file without chunkserver $i is different than written"
	fi
	saunafs_chunkserver_daemon $i start
	saunafs_wait_for_all_ready_chunkservers
done

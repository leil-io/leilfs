USE_RAMDISK=YES \
	CHUNKSERVERS=5 \
	MASTER_CUSTOM_GOALS="10 ec : \$ec(3,2)"\
	setup_local_empty_saunafs info

cd ${info[mount0]}
mkdir test
saunafs setgoal ec test

FILE_SIZE=1K file-generate test/file{1..5}

for i in {1..5}; do
	assert_success file-validate test/file$i
done;

for CS in {0..2}; do
	saunafs_chunkserver_daemon $CS stop
done

saunafs filerepair -c test/file1

for CS in {0..2}; do
	saunafs_chunkserver_daemon $CS start
done

saunafs_wait_for_all_ready_chunkservers

cd ..

# Unmount old SaunaFS client 0:
assert_success saunafs_mount_unmount 0
# Mount SaunaFS client 0:
if is_windows_system; then
	saunafs_mount_start 0
else
	assert_success saunafs_mount_start 0
fi

cd ${info[mount0]}

for i in {1..5}; do
	assert_success file-validate test/file$i
done;

for CS in {0..2}; do
	saunafs_chunkserver_daemon $CS stop
done

saunafs filerepair test/file1

for CS in {0..2}; do
	saunafs_chunkserver_daemon $CS start
done

saunafs_wait_for_all_ready_chunkservers

cd ..

# Unmount old SaunaFS client 0:
assert_success saunafs_mount_unmount 0
# Mount SaunaFS client 0:
if is_windows_system; then
	saunafs_mount_start 0
else
	assert_success saunafs_mount_start 0
fi

cd ${info[mount0]}

assert_failure file-validate test/file1

for i in {2..5}; do
	assert_success file-validate test/file$i
done;

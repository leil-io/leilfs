CHUNKSERVERS=4 \
	MASTER_CUSTOM_GOALS="20 ec31: \$ec(3,1)" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER,sfsdirentrycacheto=0" \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

# Create small files of goals 2 and ec31
cd "${info[mount0]}"
touch file{1..2}
saunafs_command setgoal 2 file1
saunafs_command setgoal ec31 file2
FILE_SIZE=1M file-generate file1
FILE_SIZE=1M file-generate file2
assert_success file-validate file*
assert_equals 6 $(find_all_metadata_chunks | wc -l)

# Create some snapshots of this file
saunafs_command makesnapshot file1 file1_snapshot1
saunafs_command makesnapshot file2 file2_snapshot1
saunafs_command makesnapshot file2 file2_snapshot2
assert_success file-validate file*
assert_equals 6 $(find_all_metadata_chunks | wc -l)

# Modify the original file and check if data in snapshots remains unchanged
dd if=/dev/zero of=file2 bs=8KiB count=100 conv=notrunc
assert_success file-validate file1* file2_snapshot*
file-overwrite file2
assert_success file-validate file*
assert_equals 10 $(find_all_metadata_chunks | wc -l)

# Truncate file2_snapshot2 up and verify if chunk was duplicated, then fix the data in this file
truncate -s 2M file2_snapshot2
assert_equals 14 $(find_all_metadata_chunks | wc -l)
file-overwrite file2_snapshot2
assert_success file-validate file*
assert_equals 14 $(find_all_metadata_chunks | wc -l)

# Truncate file1_snapshot1 down and verify if chunk was duplicated, then fix the data in this file
truncate -s 500KiB file1_snapshot1
assert_equals 16 $(find_all_metadata_chunks | wc -l)
file-overwrite file1_snapshot1
assert_success file-validate file*
assert_equals 16 $(find_all_metadata_chunks | wc -l)

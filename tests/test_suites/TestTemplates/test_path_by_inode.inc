USE_RAMDISK=YES \
	MOUNT_0_EXTRA_CONFIG="sfscachemode=NEVER,sfsdirentrycacheto=0" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"
touch file1
mkdir folder
touch folder/file2
mkdir folder/folder2
touch folder/folder2/file3

cat .saunafs_path_by_inode/2
cat .saunafs_path_by_inode/3
cat .saunafs_path_by_inode/4
cat .saunafs_path_by_inode/5
cat .saunafs_path_by_inode/6

# check file paths by inode
assert_equals "file1" "$(cat .saunafs_path_by_inode/2)"
assert_equals "folder" "$(cat .saunafs_path_by_inode/3)"
assert_equals "folder/file2" "$(cat .saunafs_path_by_inode/4)"
assert_equals "folder/folder2" "$(cat .saunafs_path_by_inode/5)"
assert_equals "folder/folder2/file3" "$(cat .saunafs_path_by_inode/6)"

# check reading from files by their path
echo "data1" > file1
echo "data2" > folder/file2
echo "data3" > folder/folder2/file3

assert_equals "data1" "$(cat $(cat .saunafs_path_by_inode/2))"
assert_equals "data2" "$(cat $(cat .saunafs_path_by_inode/4))"
assert_equals "data3" "$(cat $(cat .saunafs_path_by_inode/6))"

# check writing to files by their path
echo "data4" > $(cat .saunafs_path_by_inode/2)
echo "data5" > $(cat .saunafs_path_by_inode/4)
echo "data6" > $(cat .saunafs_path_by_inode/6)

assert_equals "data4" "$(cat file1)"
assert_equals "data5" "$(cat folder/file2)"
assert_equals "data6" "$(cat folder/folder2/file3)"

# check removing files by their path
rm $(cat .saunafs_path_by_inode/2)
assert_failure cat file1

# requesting path of removed file or unexisting inode should 
# return not such file or directory
assert_failure cat .saunafs_path_by_inode/2
assert_failure cat .saunafs_path_by_inode/1500

# the .saunafs_path_by_inode directory is not writable or removable
assert_failure rm .saunafs_path_by_inode
assert_failure touch .saunafs_path_by_inode/file

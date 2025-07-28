USE_RAMDISK=YES \
	MOUNT_0_EXTRA_CONFIG="sfscachemode=NEVER,sfsreportreservedperiod=1,sfsdirentrycacheto=0" \
	MASTER_EXTRA_CONFIG="EMPTY_RESERVED_FILES_PERIOD_MSECONDS = 3000" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"
touch file1
mkdir folder
touch folder/file2
mkdir folder/folder2
touch folder/folder2/file3
touch file4

cat .saunafs_path_by_inode/2
cat .saunafs_path_by_inode/3
cat .saunafs_path_by_inode/4
cat .saunafs_path_by_inode/5
cat .saunafs_path_by_inode/6
cat .saunafs_path_by_inode/7

# check file paths by inode
assert_equals "file1" "$(cat .saunafs_path_by_inode/2)"
assert_equals "folder" "$(cat .saunafs_path_by_inode/3)"
assert_equals "folder/file2" "$(cat .saunafs_path_by_inode/4)"
assert_equals "folder/folder2" "$(cat .saunafs_path_by_inode/5)"
assert_equals "folder/folder2/file3" "$(cat .saunafs_path_by_inode/6)"
assert_equals "file4" "$(cat .saunafs_path_by_inode/7)"

# check reading from files by their path
echo "data1" > file1
echo "data2" > folder/file2
echo "data3" > folder/folder2/file3
echo "data4" > file4

assert_equals "data1" "$(cat $(cat .saunafs_path_by_inode/2))"
assert_equals "data2" "$(cat $(cat .saunafs_path_by_inode/4))"
assert_equals "data3" "$(cat $(cat .saunafs_path_by_inode/6))"
assert_equals "data4" "$(cat $(cat .saunafs_path_by_inode/7))"

# check writing to files by their path
echo "data4" > $(cat .saunafs_path_by_inode/2)
echo "data5" > $(cat .saunafs_path_by_inode/4)
echo "data6" > $(cat .saunafs_path_by_inode/6)
echo "data7" > $(cat .saunafs_path_by_inode/7)

assert_equals "data4" "$(cat file1)"
assert_equals "data5" "$(cat folder/file2)"
assert_equals "data6" "$(cat folder/folder2/file3)"
assert_equals "data7" "$(cat file4)"

# check if root inode is accessible with the .saunafs_path_by_inode
# special file with id 1 and real underlying id on the filesystem
# when mounting client on a sfssubfolder created before
cd ..
saunafs_mount_unmount 0
echo "sfssubfolder=folder/folder2" >> "${info[mount0_cfg]}"
if is_windows_system; then
	saunafs_mount_start 0
else
	assert_success saunafs_mount_start 0
fi
cd "${info[mount0]}"

assert_equals "" "$(cat .saunafs_path_by_inode/1)"
assert_equals "" "$(cat .saunafs_path_by_inode/5)"

# check that accessing a file path by inode inside the 
# mounted subfolder will return the corresponding path
# without including the subfolder as part of it.
assert_equals "file3" "$(cat .saunafs_path_by_inode/6)"

# check that it is not possible to access the path by inode
# result of inodes outside the mounted subfolder that were
# created before or after it.
assert_failure cat .saunafs_path_by_inode/4 # corresponding to folder/file2
assert_failure cat .saunafs_path_by_inode/7 # corresponding to file4

# mount client back to initial root folder
cd ..
saunafs_mount_unmount 0
echo "sfssubfolder=/" >> "${info[mount0_cfg]}"
if is_windows_system; then
	saunafs_mount_start 0
else
	assert_success saunafs_mount_start 0
fi
cd "${info[mount0]}"

# check removing files by their path
# first check the case when the file was deleted but
# is still on the filesystem trash
saunafs settrashtime 2 file1
rm file1
assert_failure cat file1
assert_success cat .saunafs_path_by_inode/2
assert_equals "file1 (trash)" "$(cat .saunafs_path_by_inode/2)"
sleep 5

# requesting path of removed file (after trashtime expired) or 
# unexisting inode should return not such file or directory
assert_failure cat .saunafs_path_by_inode/2
assert_failure cat .saunafs_path_by_inode/1500

# the .saunafs_path_by_inode directory is not writable or removable
assert_failure rm .saunafs_path_by_inode
assert_failure touch .saunafs_path_by_inode/file

# check removing a file while still in use and with no
# trashtime is sending it to the reserved files
saunafs settrashtime 0 file4
while true; do echo "Data" >> file4; sleep 1; done &
rm file4
assert_failure cat file4
assert_success cat .saunafs_path_by_inode/7
assert_equals "file4 (reserved)" "$(cat .saunafs_path_by_inode/7)"

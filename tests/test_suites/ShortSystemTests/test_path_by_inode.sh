source test_suites/TestTemplates/test_path_by_inode.inc

assert_success cat .saunafs_path_by_inode/4
chmod 200 folder/file2
assert_failure cat .saunafs_path_by_inode/4

# if a user has no reading permissions to a parent folder, 
# then all path from all possible children inodes cannot 
# be accessed
assert_success cat .saunafs_path_by_inode/3
assert_success cat .saunafs_path_by_inode/5
assert_success cat .saunafs_path_by_inode/6

chmod 200 folder

assert_failure cat .saunafs_path_by_inode/3
assert_failure cat .saunafs_path_by_inode/5
assert_failure cat .saunafs_path_by_inode/6

# test path by inode for softlinks and hardlinks
touch file5
echo "data_file5" > file5

ln -s file5 softlink_to_file
ln file5 hardlink_to_file

assert_equals "file5" "$(cat .saunafs_path_by_inode/8)"
assert_equals "data_file5" "$(cat $(cat .saunafs_path_by_inode/8))"

# check that inode of softlink is a new one
file5_inode=$(stat -c %i file5)
softlink_inode=$(stat -c %i softlink_to_file)
assert_not_equal "$softlink_inode" "$file5_inode"
assert_equals "softlink_to_file" "$(cat .saunafs_path_by_inode/9)"
assert_equals "data_file5" "$(cat $(cat .saunafs_path_by_inode/9))"

# check that inode of hardlink is the same as the original file
file5_hardlink_inode=$(stat -c %i hardlink_to_file)
assert_equals "$file5_inode" "$file5_hardlink_inode"

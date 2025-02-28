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

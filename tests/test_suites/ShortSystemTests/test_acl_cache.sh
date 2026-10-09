assert_program_installed setfacl getfacl

file1_acl='user::rw- user:saunafstest_1:r-x group::rw- group:saunafstest_4:--- mask::rwx other::-wx'
file2_acl='user::rw- user:saunafstest_2:rwx group::rw- group:saunafstest_5:-w- mask::rwx other::r-x'
file3_acl='user::rw- user:saunafstest_3:-w- group::rw- group:saunafstest_6:rwx mask::rwx other::r--'
file4_acl='user::rw- user:saunafstest_7:r-- group::rw- group:saunafstest_8:--x mask::rwx other::rw-'

count_misses() {
	cat "$TEMP_DIR/aclcache.log" | grep "master.cltoma_fuse_getacl: $(stat -c %i $1)" | wc -l
}

check_misses() {
	assert_equals "$1" "$(count_misses file1)"
	assert_equals "$2" "$(count_misses file2)"
	assert_equals "$3" "$(count_misses file3)"
}

function get_facl() {
	file=$1
	getfacl -cpE "$file" | tr "\n" " " | trim
}

# On Ubuntu 26.04 the parent directory's ACL also lands in this cache, taking
# a slot; sizes below keep the expected misses the same either way.
CHUNKSERVERS=1 \
	USE_RAMDISK=YES \
	SFSEXPORTS_EXTRA_OPTIONS=nomasterpermcheck,ignoregid \
	MASTER_EXTRA_CONFIG="MAGIC_DEBUG_LOG = $TEMP_DIR/aclcache.log|LOG_FLUSH_ON=TRACE" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER|sfsaclcachesize=3|sfsaclcacheto=5.0|sfsattrcacheto=50" \
	setup_local_empty_saunafs info

cd ${info[mount0]}

# Create files, set ACLs for them
touch file1 file2 file3 file4
chmod 664 file*
setfacl -m u:saunafstest_1:r-x -m g:saunafstest_4:--- -m o::-wx file1
setfacl -m u:saunafstest_2:rwx -m g:saunafstest_5:-w- -m o::r-x file2
setfacl -m u:saunafstest_3:-w- -m g:saunafstest_6:rwx -m o::r-- file3
setfacl -m u:saunafstest_7:r-- -m g:saunafstest_8:--x -m o::rw- file4
truncate -s0 "$TEMP_DIR/aclcache.log"

# Load data into cache
assert_equals "$(get_facl file1)" "$file1_acl"
assert_equals "$(get_facl file2)" "$file2_acl"
check_misses 1 1 0

# Read ACL, expect to hit the cache...
assert_equals "$(get_facl file1)" "$file1_acl"
assert_equals "$(get_facl file2)" "$file2_acl"
check_misses 1 1 0
sleep 3
assert_equals "$(get_facl file1)" "$file1_acl"
assert_equals "$(get_facl file2)" "$file2_acl"
check_misses 1 1 0

# ... until the timeout is exceeded:
sleep 2
assert_equals "$(get_facl file1)" "$file1_acl"
assert_equals "$(get_facl file2)" "$file2_acl"
check_misses 2 2 0

# Check if the cache size limit works:
for file in file3 file4 file1 file2 file3 file4 file1 file2; do
	acl_var="${file}_acl"
	assert_equals "$(get_facl "$file")" "${!acl_var}"
done
check_misses 4 4 2

# Check if the cache is invalidated after setfacl
setfacl -b file1
assert_equals "$(get_facl file1)" "user::rw- group::rw- other::-wx"
check_misses 5 4 2

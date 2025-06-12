CHUNKSERVERS=4 \
	SFSEXPORTS_EXTRA_OPTIONS="allcanchangequota" \
	# MASTER_CUSTOM_GOALS="2 2: _" \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

touch test
mkdir dir

goal=$(saunafs getgoal dir)

assert_equals "$goal" "dir: 1"

goal=$(saunafs setgoal 2 dir)

assert_equals "$goal" "dir: 2"


time=$(saunafs gettrashtime test)
assert_equals "$time" "test: 94620"
saunafs settrashtime 0 test
time=$(saunafs gettrashtime test)
assert_equals "$time" "test: 0"

attrs=$(saunafs geteattr test)
assert_equals "$attrs" "test: -"
saunafs seteattr -f noattrcache test
attrs=$(saunafs geteattr test)
assert_equals "$attrs" "test: noattrcache"
saunafs deleattr -f noattrcache test
attrs=$(saunafs geteattr test)
assert_equals "$attrs" "test: -"

check=$(saunafs checkfile test)
assert_equals "$check" "test:"
check=$(saunafs fileinfo test)
assert_equals "$check" "test:"

echo "foo" > test
echo "bar" > test2

saunafs appendchunks test2 test
# We aren't checking the result, since it can take some time. Another test
# should be check if it's working with (with the saunafs command)

assert_success saunafs dirinfo dir

saunafs filerepair test

saunafs makesnapshot dir dir_snapshot

quota=$(saunafs repquota -d dir)
assert_equals "$quota" "# User/Group ID/Directory; Bytes: current usage, soft limit, hard limit; Inodes: current usage, soft limit, hard limit;"

saunafs setquota -d 1000000 2000 1000000 2000 dir

quota=$(saunafs repquota -d dir)
assert_equals "$quota" "$(saunafs repquota -d dir)"

# Create a 25 chunks file and make a 4x snapshot of it
FILE_SIZE=1600M file-generate test3
saunafs appendchunks 4xtest3 test3 test3 test3 test3
assert_equals $((4*25)) $(saunafs fileinfo 4xtest3 | grep "chunk" | wc -l)
assert_equals $((4*25*64*1024*1024)) $(saunafs dirinfo 4xtest3 | grep "length:" | awk '{print $2}')

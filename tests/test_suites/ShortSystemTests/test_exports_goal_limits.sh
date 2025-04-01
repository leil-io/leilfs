CHUNKSERVERS=1 \
	USE_RAMDISK=YES \
	MOUNTS=4 \
	MOUNT_1_EXTRA_EXPORTS="mingoal=2" \
	MOUNT_2_EXTRA_EXPORTS="maxgoal=14" \
	MOUNT_3_EXTRA_EXPORTS="mingoal=10,maxgoal=12" \
	setup_local_empty_saunafs info

# Create some directory tree to work with
mkdir -p "${info[mount0]}"/dir{1..4}/sub{1..4}

cd "${info[mount1]}"
assert_success saunafs setgoal     2 dir1/sub1
assert_success saunafs setgoal -r  3 dir1/sub2
assert_success saunafs setgoal     4 dir1/sub3
assert_success saunafs setgoal -r  2 dir1
assert_success saunafs setgoal -r  7 dir1
assert_success saunafs setgoal    13 dir1
assert_success saunafs setgoal -r 20 dir1
assert_failure saunafs setgoal     1 dir1/sub4 # Too low!
assert_failure saunafs setgoal -r  1 dir1      # Too low!

cd "${info[mount2]}"
assert_success saunafs setgoal -r 13 dir1
assert_success saunafs setgoal     1 dir2/sub1
assert_success saunafs setgoal -r  4 dir2/sub1
assert_success saunafs setgoal     9 dir2/sub1
assert_success saunafs setgoal -r 13 dir2/sub1
assert_success saunafs setgoal    14 dir2/sub1
assert_failure saunafs setgoal -r 15 dir2/sub2 # Too high!
assert_failure saunafs setgoal    17 dir2/sub3 # Too high!
assert_failure saunafs setgoal    20 dir2/sub4 # Too high!
assert_success saunafs setgoal -r  9 dir2
assert_success saunafs setgoal -r 13 dir2
assert_success saunafs setgoal    14 dir2
assert_failure saunafs setgoal -r 15 dir2      # Too high!

cd "${info[mount3]}"
assert_failure saunafs setgoal     4 dir3/sub1 # Too low!
assert_failure saunafs setgoal -r  9 dir3/sub1 # Too low!
assert_success saunafs setgoal    10 dir3/sub1
assert_success saunafs setgoal -r 11 dir3/sub1
assert_success saunafs setgoal    12 dir3/sub1
assert_failure saunafs setgoal -r 13 dir3/sub1 # Too high!
assert_failure saunafs setgoal    16 dir3/sub1 # Too high!
assert_success saunafs setgoal -r 12 dir3/sub1
assert_success saunafs setgoal    10 dir3
assert_failure saunafs setgoal -r  9 dir3      # Too low!
assert_success saunafs setgoal -r 12 dir3
assert_failure saunafs setgoal    13 dir3      # Too high!
assert_failure saunafs setgoal -r 14 dir3      # Too high!
assert_failure saunafs setgoal    20 dir3      # Too high!

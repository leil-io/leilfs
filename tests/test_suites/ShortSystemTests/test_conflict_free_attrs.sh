timeout_set '2 minutes'

# Conflict-free attributes (dir nlink via DIR_SUBDIRS_, dir mtime via DIR_CHILD_CHANGE_TIME_,
# atime via NODE_ATIME_) must read back identically on the in-memory Master and the KV MDS, so
# this test runs in both ShortSystemTests and FDBTests (test_short_ wrapper) and survives restart.

USE_RAMDISK="YES" \
	CHUNKSERVERS=1 \
	MOUNTS=2 \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

cd "${info[mount0]}" || exit 1

# drop_caches before each stat: FUSE caches attributes, which would otherwise mask a
# server-side timestamp/nlink change.

# Directory link count: POSIX 2 ("." + parent entry) + one per direct subdirectory
mkdir d
drop_caches
assert_equals 2 "$(stat -c %h d)"

mkdir d/sub1 d/sub2 d/sub3
drop_caches
assert_equals 5 "$(stat -c %h d)"

# Plain files are not subdirectories: they must not change the link count.
touch d/file1 d/file2
drop_caches
assert_equals 5 "$(stat -c %h d)"

# Removing a subdirectory decrements it.
rmdir d/sub3
drop_caches
assert_equals 4 "$(stat -c %h d)"

# Directory mtime advances when a child is added (1s timestamp granularity)
mtime_before=$(stat -c %Y d)
sleep 1.2
touch d/file3
drop_caches
mtime_after=$(stat -c %Y d)
assert_less_than "${mtime_before}" "${mtime_after}"

# Explicit directory mtime changes can move backward and must replace the max shadow.
dir_past_mtime=$(( $(date +%s) - 100000 ))
touch -m -d "@${dir_past_mtime}" d
drop_caches
assert_equals "${dir_past_mtime}" "$(stat -c %Y d)"

# An explicit future dir mtime must not drag ctime forward: the child-change shadow (also used
# to reconcile ctime) is clamped to the change time, so mtime follows the future value, ctime = now.
dir_future_mtime=$(( $(date +%s) + 100000 ))
touch -m -d "@${dir_future_mtime}" d
drop_caches
assert_equals "${dir_future_mtime}" "$(stat -c %Y d)"
assert_less_than "$(stat -c %Z d)" "${dir_future_mtime}"

# atime advances through the conflict-free read path.
# Use a nonempty file so cat reaches persistNodeAtime and the atomicMax shadow.
printf 'payload\n' > read_file
read_past_atime=$(( $(date +%s) - 100000 ))
touch -a -d "@${read_past_atime}" read_file
drop_caches
cat "${info[mount1]}/read_file" > /dev/null
drop_caches
read_advanced_atime=$(stat -c %X read_file)
assert_less_than "${read_past_atime}" "${read_advanced_atime}"

# Explicit atime changes can move backward and must replace the max shadow.
touch reset_file
future_atime=$(( $(date +%s) + 100000 ))
reset_past_atime=$(( $(date +%s) - 100000 ))
touch -a -d "@${future_atime}" reset_file
touch -a -d "@${reset_past_atime}" reset_file
drop_caches
assert_equals "${reset_past_atime}" "$(stat -c %X reset_file)"

# Everything survives one MDS restart. The KV backend reloads the shadows from FDB;
# the in-memory Master reloads its metadata dump.
dir_nlink=$(stat -c %h d)
dir_mtime=$(stat -c %Y d)

saunafs_master_daemon restart
saunafs_wait_for_all_ready_chunkservers
drop_caches

assert_equals "${dir_nlink}" "$(stat -c %h d)"
assert_equals "${dir_mtime}" "$(stat -c %Y d)"
assert_equals "${read_advanced_atime}" "$(stat -c %X read_file)"
assert_equals "${reset_past_atime}" "$(stat -c %X reset_file)"

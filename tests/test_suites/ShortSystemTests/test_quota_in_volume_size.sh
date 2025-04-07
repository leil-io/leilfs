timeout_set 40 seconds

CHUNKSERVERS=1 \
	USE_RAMDISK=YES \
	SFSEXPORTS_EXTRA_OPTIONS="allcanchangequota" \
	MOUNT_EXTRA_CONFIG="usequotainvolumesize=1,statfscachetimeout=1,sfsreportreservedperiod=1" \
	setup_local_empty_saunafs info

get_mountpoint_total_space() {
	df --block-size=1 | grep "${info[mount0]}" | awk '{print $2}'
}

get_mountpoint_available_space() {
	df --block-size=1 | grep "${info[mount0]}" | awk '{print $4}'
}

cd "${info[mount0]}"

mkdir dir
saunafs settrashtime 0 dir

# Restart the mountpoint on the subfolder dir
cd $TEMP_DIR
saunafs_mount_unmount 0
echo "sfssubfolder=dir" >> "${info[mount0_cfg]}"
assert_success saunafs_mount_start 0

cd "${info[mount0]}"

user_limit_mb=192
user_limit=$((user_limit_mb*1024*1024))
current_total_space=$(get_mountpoint_total_space)
assert_less_than ${user_limit} ${current_total_space}
echo "OK: the total space is more than ${user_limit_mb}MiB"

# Set the quota to 192MiB for current user (saunafstest)
saunafs setquota -u $(id -u) 0 ${user_limit} 0 0 .
assert_success dd if=/dev/zero of=file1 bs=1M count=$((user_limit_mb))
assert_equals $(get_mountpoint_total_space) ${user_limit}
echo "Ok: the total space is exactly ${user_limit_mb}MiB"
# Check the available space is exactly zero
assert_equals $(get_mountpoint_available_space) 0
echo "Ok: the available space is exactly zero"

# Check the quota is actually enforced
assert_failure dd if=/dev/zero of=file1 bs=1M count=$((user_limit_mb+1))

# Check the tweaks is working for UseQuotaInVolumeSize
assert_equals $(cat .saunafs_tweaks | grep "UseQuotaInVolumeSize" | awk '{print $2}') "true"
echo "UseQuotaInVolumeSize=false" | sudo tee ${info[mount0]}/.saunafs_tweaks
current_total_space=$(get_mountpoint_total_space)
assert_less_than ${user_limit} ${current_total_space}
echo "OK: the total space is more than ${user_limit_mb}MiB"
assert_equals $(cat .saunafs_tweaks | grep "UseQuotaInVolumeSize" | awk '{print $2}') "false"

# Brought it back
echo "UseQuotaInVolumeSize=true" | sudo tee ${info[mount0]}/.saunafs_tweaks

# Check the tweaks is working for StatfsCacheTimeout
assert_equals $(cat .saunafs_tweaks | grep "StatfsCacheTimeout" | awk '{print $2}') 1
# Set the timeout to 5 seconds
echo "StatfsCacheTimeout=5000" | sudo tee ${info[mount0]}/.saunafs_tweaks
assert_equals $(cat .saunafs_tweaks | grep "StatfsCacheTimeout" | awk '{print $2}') 5000
# Last statfs call should be still cached
assert_equals ${current_total_space} $(get_mountpoint_total_space)
echo "OK: the total space is the old one (cache is still valid)"

# Wait for the cache to expire
sleep 6
assert_equals ${user_limit} $(get_mountpoint_total_space)
echo "OK: the total space is exactly ${user_limit_mb}MiB (cache is expired)"
echo "StatfsCacheTimeout=0" | sudo tee ${info[mount0]}/.saunafs_tweaks

# Check the behavior for group quota
saunafs setquota -u $(id -u) 0 $((2*user_limit)) 0 0 .
saunafs setquota -g $(id -g) 0 $((2*user_limit)) 0 0 .
assert_success sudo -nu saunafstest_0 dd if=/dev/zero of=file2 bs=1M count=$((user_limit_mb))
sudo -nu saunafstest_0 chown saunafstest_0:$(id -g) file2

# Available space should be 0 because of the group quota: file1 and file2
# belong to the same group and there is no more space left for that group.

# Check the available space is exactly zero
assert_equals $(get_mountpoint_available_space) 0
echo "Ok: the available space is exactly zero"

# Delete the files to free up space
rm -f file1 file2

sleep 2

# Set user and group quotas to user_limit
saunafs setquota -u $(id -u) 0 $((user_limit)) 0 0 .
saunafs setquota -g $(id -g) 0 $((user_limit)) 0 0 .

# Check the behavior for folder quota
# Test increasing directory quota beyond user quota
# Create a folder with a quota of 1/2 user_limit
mkdir folder2
saunafs setquota -d 0 $((user_limit / 2)) 0 0 folder2

# Verify folder quota is enforced
assert_success dd if=/dev/zero of=folder2/file4 bs=1M count=$((user_limit_mb / 2))
assert_failure dd if=/dev/zero of=folder2/file4 bs=1M count=$((user_limit_mb))

# Increase folder quota to exceed user quota
saunafs setquota -d 0 $((2 * user_limit)) 0 0 folder2

# Verify directory quota is enforced
assert_failure dd if=/dev/zero of=folder2/file4 bs=1M count=$((user_limit_mb + 1))

# Check the available space is exactly zero
assert_equals $(get_mountpoint_available_space) 0
echo "Ok: the available space is exactly zero"

# Delete the files to free up space
rm -r folder2

sleep 2

# Set user and group quotas to user_limit
saunafs setquota -u $(id -u) 0 $((user_limit)) 0 0 .
saunafs setquota -g $(id -g) 0 $((user_limit)) 0 0 .

# Test directory quota on nested directories
# Create nested folders with different quotas
mkdir -p folder3/subfolder1
saunafs setquota -d 0 $((user_limit * 3 / 4)) 0 0 folder3
saunafs setquota -d 0 $((user_limit / 2)) 0 0 folder3/subfolder1

# Verify quotas are enforced at each level
assert_success dd if=/dev/zero of=folder3/file5 bs=1M count=$((user_limit_mb / 2))
assert_success dd if=/dev/zero of=folder3/subfolder1/file6 bs=1M count=$((user_limit_mb / 4))
assert_failure dd if=/dev/zero of=folder3/subfolder1/file6 bs=1M count=$((user_limit_mb / 2))

# Last operation should have failed after writing a full chunk
# Available space should (user_limit_mb - user_limit_mb / 2 - 64)MB
assert_less_than $(get_mountpoint_available_space) \
    $((user_limit - user_limit / 2 - SAUNAFS_CHUNK_SIZE + 1))
echo "Ok: the available space is as expected"

# Delete the files to free up space
rm -r folder3/subfolder1 folder3

sleep 2

# Set user and group quotas to user_limit
saunafs setquota -u $(id -u) 0 $((user_limit)) 0 0 .
saunafs setquota -g $(id -g) 0 $((user_limit)) 0 0 .

# Test decreasing directory quota
# Create a folder and fill it up to its quota
mkdir folder4
saunafs setquota -d 0 $((user_limit / 2)) 0 0 folder4
assert_success dd if=/dev/zero of=folder4/file7 bs=1M count=$((user_limit_mb / 2))

# Decrease the folder quota
saunafs setquota -d 0 $((user_limit / 4)) 0 0 folder4

# Verify writing further data fails
assert_failure dd if=/dev/zero of=folder4/file8 bs=1M count=1

# Check the available space is around half the user limit
assert_less_than $(get_mountpoint_available_space) $((user_limit / 2 + 1))
echo "Ok: the available space is as expected"

# Delete the files to free up space
rm -r folder4

sleep 2

# Set user and group quotas to user_limit
saunafs setquota -u $(id -u) 0 $((user_limit)) 0 0 .
saunafs setquota -g $(id -g) 0 $((user_limit)) 0 0 .

# Test directory quota with different users
# Create a folder and set a quota
mkdir folder5
saunafs setquota -d 0 $((user_limit / 2)) 0 0 folder5

# Verify the quota is enforced for different users
assert_success dd if=/dev/zero of=folder5/file9 bs=1M count=$((user_limit_mb / 4))
assert_failure sudo -nu saunafstest_0 dd if=/dev/zero of=folder5/file10 bs=1M count=$((user_limit_mb / 2))
sudo -nu saunafstest_0 chown saunafstest_0:$(id -g) folder5/file10

# Current user available space should be around 3/4 of the user limit
assert_less_than $(get_mountpoint_available_space) $((user_limit * 3 / 4 + 1))
echo "Ok: the available space is as expected"

# Delete the files to free up space
rm -r folder5

sleep 2

# Test directory quota on a mounted subfolder
# Increase the quota for user and group
saunafs setquota -u $(id -u) 0 $((3 * user_limit)) 0 0 .
saunafs setquota -g $(id -g) 0 $((3 * user_limit)) 0 0 .

# Create a folder with a quota of 2*user_limit which 
# is smaller than user and group quotas
mkdir folder6
saunafs setquota -d 0 $((2 * user_limit)) 0 0 folder6

# Restart the mountpoint on the subfolder dir/folder6
cd $TEMP_DIR
saunafs_mount_unmount 0
echo "sfssubfolder=dir/folder6" >> "${info[mount0_cfg]}"
assert_success saunafs_mount_start 0

cd "${info[mount0]}"

#after we mounted dir/folder6 we expect that reported total space is equal to the 
#min(user quota, group quota, directory quota) 
#min(3*user_limit, 3*user_limit, 2*user_limit) = 2*user_limit
assert_equals $(get_mountpoint_total_space) $((2 * user_limit))

# Create a file in the folder
assert_success dd if=/dev/zero of=file13 bs=1M count=$((2 * user_limit_mb))

# # Available space should be 0 because of the folder quota: file13 is in folder6
assert_equals 0 $(get_mountpoint_available_space)
echo "Ok: the available space is exactly zero"

timeout_set 20 seconds

CHUNKSERVERS=1 \
	USE_RAMDISK=YES \
	SFSEXPORTS_EXTRA_OPTIONS="allcanchangequota" \
	MOUNT_EXTRA_CONFIG="usequotainvolumesize=1,statfscachetimeout=1" \
	setup_local_empty_saunafs info

get_mountpoint_total_space() {
	df --block-size=1 | grep "${info[mount0]}" | awk '{print $2}'
}

get_mountpoint_available_space() {
	df --block-size=1 | grep "${info[mount0]}" | awk '{print $4}'
}

cd "${info[mount0]}"

user_limit_mb=512
user_limit=$((user_limit_mb*1024*1024))
current_total_space=$(get_mountpoint_total_space)
assert_less_than ${user_limit} ${current_total_space}
echo "OK: the total space is more than ${user_limit_mb}MiB"

# Set the quota to 512MiB for current user (saunafstest)
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

# The goal of this test is to make sure that client parsing is able to process option
# names that use capital letters instead of the expected lowercase letters. At the end
# it also checks that poorly written config files will make client to fail its mount
# instead of ignoring them.

CHUNKSERVERS=1 \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

# Expected values are current defaults
assert_equals $(cat .saunafs_tweaks | egrep CacheExpirationTime | awk '{print $2}') 1000
assert_equals $(cat .saunafs_tweaks | egrep WriteMaxRetries | awk '{print $2}') 30
assert_equals $(cat .saunafs_tweaks | egrep MaxReadaheadRequests | awk '{print $2}') 2
assert_equals $(cat .saunafs_tweaks | egrep ReadWaveTimeout | awk '{print $2}') 500

cd /
saunafs_mount_unmount 0

# Must chose some values different than current defaults
echo "-o CacheExpirationTime=2000" >> "${info[mount0_cfg]}"
echo "sfsIORetries=20" >> "${info[mount0_cfg]}"
echo "-oMaxReadaheadRequests=2,SFSChUnKSerVerWaVEREadtO=200" >> "${info[mount0_cfg]}"

saunafs_mount_start 0
cd "${info[mount0]}"

assert_equals $(cat .saunafs_tweaks | egrep CacheExpirationTime | awk '{print $2}') 2000
assert_equals $(cat .saunafs_tweaks | egrep WriteMaxRetries | awk '{print $2}') 20
assert_equals $(cat .saunafs_tweaks | egrep MaxReadaheadRequests | awk '{print $2}') 2
assert_equals $(cat .saunafs_tweaks | egrep ReadWaveTimeout | awk '{print $2}') 200

cd /
saunafs_mount_unmount 0

# Try writing some error in a cfg file ...
echo "-o foo=bar" >> "${info[mount0_cfg]}"

assert_failure ${saunafs_info_[mntcall0]}

# Let's substitute the wrong option for some other, which is not expected here (like --help) ...
sed -i 's/-o foo=bar/--help/g' "${info[mount0_cfg]}"

assert_failure ${saunafs_info_[mntcall0]}

timeout_set "40 seconds"
MOUNT_EXTRA_CONFIG="sfsnegativecachesize=4,sfsnegativecachetimeout=9000" \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

cd ${info[mount0]}

# Tweaks test
expect_equals "9000" "$(cat ${info[mount0]}/.saunafs_tweaks | grep -i negativecachetimeout | awk '{print $2}')"
expect_equals "4" "$(cat ${info[mount0]}/.saunafs_tweaks | grep -i negativecachemaxsize | awk '{print $2}')"

echo "NegativeCacheTimeout=9001" | sudo tee ${info[mount0]}/.saunafs_tweaks
echo "NegativeCacheMaxSize=5" | sudo tee ${info[mount0]}/.saunafs_tweaks

expect_equals "9001" "$(cat ${info[mount0]}/.saunafs_tweaks | grep -i negativecachetimeout | awk '{print $2}')"
expect_equals "5" "$(cat ${info[mount0]}/.saunafs_tweaks | grep -i negativecachemaxsize | awk '{print $2}')"

# Timeout test
test_file=nonexistent_file
assert_failure ls ${test_file}

touch ${test_file}
assert_failure ls ${test_file}

assert_eventually "ls ${test_file}" "20 seconds"

# LRU eviction test, remove oldest when exceeds max size
files1=({a..f}) # 6 files

# When file f is added, a is removed because it is oldest
for file in ${files1[@]}; do assert_failure ls ${file}; done
touch ${files1[0]}
assert_success ls ${files1[0]}

# Tweaks change clears whole cache
files2=({A..C})
for file in ${files2[@]}; do assert_failure ls ${file}; done
echo "NegativeCacheTimeout=9000" | sudo tee ${info[mount0]}/.saunafs_tweaks
assert_failure ls D # clears caches
touch ${files2[@]}
for file in ${files2[@]}; do assert_success ls ${file}; done
rm ${files2[@]}

for file in ${files2[@]}; do assert_failure ls ${file}; done
echo "NegativeCacheMaxSize=4" | sudo tee ${info[mount0]}/.saunafs_tweaks
assert_failure ls E # clears caches
touch ${files2[@]}
for file in ${files2[@]}; do assert_success ls ${file}; done

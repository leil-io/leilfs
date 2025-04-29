timeout_set 40 seconds

check_stat() {
    dir=$1
    total_expected=$2
    free_expected=$3

    stat_output=$(stat -f "$dir")
    block_size=$(echo "$stat_output" | grep "Block size" | awk '{print $3}')
    blocks_total=$(echo "$stat_output" | grep "Blocks: Total" | awk '{print $3}')
    blocks_free=$(echo "$stat_output" | grep "Blocks: Free" | awk '{print $3}')

    total_calculated=$((total_expected / block_size))
    free_calculated=$((free_expected / block_size))

    echo "Directory: $dir"
    echo "Expected Blocks Total: $total_calculated, Actual: $blocks_total"
    echo "Expected Blocks Free: $free_calculated, Actual: $blocks_free"
}

CHUNKSERVERS=1 \
	USE_RAMDISK=YES \
	SFSEXPORTS_EXTRA_OPTIONS="allcanchangequota" \
	MOUNT_EXTRA_CONFIG="usequotainvolumesize=1,statfscachetimeout=1,sfsreportreservedperiod=1" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

# Test stat command updated capacity
mkdir -p ./app1 ./app2/nested
saunafs setquota -d 0 1G 0 0 ./app1
saunafs setquota -d 0 2G 0 0 ./app2
head -c 100M /dev/random > ./app1/app1_file
head -c 100M /dev/random > ./app2/app2_file
head -c 100M /dev/random > ./app2/nested/app2_nested_file

stat -f ./app1
stat -f ./app2
stat -f ./app2/nested

# Check ./app1, expect 1GB total and 900MB free
check_stat ./app1 $((1024 * 1024 * 1024)) $((900 * 1024 * 1024))

# Check ./app2, expect 2GB total and 1800MB free
check_stat ./app2 $((2 * 1024 * 1024 * 1024)) $((1800 * 1024 * 1024))

# Check ./app2/nested, expect 2GB total and 1800MB free
check_stat ./app2/nested $((2 * 1024 * 1024 * 1024)) $((1800 * 1024 * 1024))

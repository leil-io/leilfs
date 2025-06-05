timeout_set 40 seconds

CHUNKSERVERS=1 \
    USE_RAMDISK=YES \
    SFSEXPORTS_EXTRA_OPTIONS="allcanchangequota" \
    MOUNT_EXTRA_CONFIG="usequotainvolumesize=1,statfscachetimeout=1,sfsreportreservedperiod=1,sfsuseinodebasedwritealgorithm=1" \
    setup_local_empty_saunafs info

get_folder_total_space() {
    stat -f "${info[mount0]}/$1" | awk '
        /Block size:/ {blocksize=$3}
        /Blocks: Total:/ {total_blocks=$3}
        END {
            print blocksize * total_blocks
        }'
}

get_folder_available_space() {
    stat -f "${info[mount0]}/$1" | awk '
        /Block size:/ {blocksize=$3}
        /Available:/ {available_blocks=$3}
        END {
            print blocksize * available_blocks
        }'
}

cd "${info[mount0]}"

# Create two test folders
mkdir folderA folderB

user_limit_mb=192
user_limit=$((user_limit_mb * 1024 * 1024))
quotaA=$((user_limit / 3))
quotaB=$((user_limit / 2))

# Set quotas for the two folders
saunafs setquota -d 0 $quotaA 0 0 folderA
saunafs setquota -d 0 $quotaB 0 0 folderB

echo "Quotas set: folderA=$quotaA, folderB=$quotaB"

# Verify folder available space is the minimum of the two quotas (A < B, so A is expected)
assert_equals "$quotaA" "$(get_folder_available_space folderA)"
echo "OK: folderA available space matches its quota: $quotaA"

assert_equals "$quotaB" "$(get_folder_available_space folderB)"
echo "OK: folderB available space matches its quota : $quotaB"

# Check the tweaks is working for UseQuotaInVolumeSize
assert_equals "$(cat .saunafs_tweaks | grep "UseQuotaInVolumeSize" | awk '{print $2}')" "true"
echo "UseQuotaInVolumeSize=false" | sudo tee "${info[mount0]}/.saunafs_tweaks"

# Verify total space reflects the full available space when UseQuotaInVolumeSize is disabled
current_total_space=$(get_folder_total_space folderA)
echo "OK: quotaA: ${quotaA}, current total space: ${current_total_space}"
assert_less_than "${quotaA}" "${current_total_space}"
current_total_space=$(get_folder_total_space folderB)
echo "OK: quotaB: ${quotaB}, current total space: ${current_total_space}"
assert_less_than "${quotaB}" "${current_total_space}"
echo "OK: UseQuotaInVolumeSize disabled, total space reflects full available space."

# Re-enable UseQuotaInVolumeSize
echo "UseQuotaInVolumeSize=true" | sudo tee "${info[mount0]}/.saunafs_tweaks"
assert_equals "$(cat .saunafs_tweaks | grep "UseQuotaInVolumeSize" | awk '{print $2}')" "true"

# Set StatfsCacheTimeout to 5 seconds
echo "StatfsCacheTimeout=5000" | sudo tee "${info[mount0]}/.saunafs_tweaks"
assert_equals "$(cat .saunafs_tweaks | grep "StatfsCacheTimeout" | awk '{print $2}')" 5000

# Verify cached values are still returned before cache expiration for folderA
cached_total_space=$(get_folder_total_space folderA)
echo "Cached total space for folderA: ${cached_total_space}, current total space: ${current_total_space}"
assert_equals "${current_total_space}" "${cached_total_space}"

# Verify cached values are still returned before cache expiration for folderB
cached_total_space=$(get_folder_total_space folderB)
echo "Cached total space for folderB: ${cached_total_space}, current total space: ${current_total_space}"
assert_equals "${current_total_space}" "${cached_total_space}"

echo "OK: Cached total space is still valid before cache expiration"

# Wait for the cache to expire
sleep 6

# Verify total space is updated to reflect quotas after cache expiration for folderA
updated_total_space=$(get_folder_total_space folderA)
echo "Updated total space for folderA after cache expiration: ${updated_total_space}, quotaA: ${quotaA}"
assert_equals "${quotaA}" "${updated_total_space}"

# Verify total space is updated to reflect quotas after cache expiration for folderB
updated_total_space=$(get_folder_total_space folderB)
echo "Updated total space for folderB after cache expiration: ${updated_total_space}, quotaB: ${quotaB}"
assert_equals "${quotaB}" "${updated_total_space}"

echo "OK: Statfs cache expired, total space updated to reflect quotas"

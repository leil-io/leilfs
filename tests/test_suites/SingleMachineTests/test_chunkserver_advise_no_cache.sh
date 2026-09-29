# This test checks if the HDD_ADVISE_NO_CACHE configuration option works as
# expected. When HDD_ADVISE_NO_CACHE is set to 1, the chunkserver should not
# cache the chunks in the memory. The test uses two approaches, the system
# memory and the cached pages used by the data parts of the chunks (using the
# vmtouch command).
#
# Not fincore: since util-linux 2.41 it uses cachestat(2), which always
# reports 0 cached pages for files on overlayfs (e.g. /mnt/hdd_* inside the
# Docker test containers). vmtouch uses mmap + mincore(2), which sees the
# real file's page cache through the overlay.

assert_program_installed vmtouch

timeout_set 2 minutes

CHUNKSERVERS=1 \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	CHUNKSERVER_EXTRA_CONFIG="HDD_ADVISE_NO_CACHE = 0" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

function getSystemCachedMemorySizeKiB {
	grep '^Cached' /proc/meminfo | awk '{print $2}'
}

function getCachedMemoryUsedByChunksKiB {
	hdd_data=$(get_data_path "${info[chunkserver0_hdd]}")
	chunks=$(find "$hdd_data" -name "chunk_*${chunk_data_extension}")

	local totalPages=0

	for chunk in ${chunks}; do
		local pages=$(vmtouch "${chunk}" | awk '/Resident Pages/ {split($3, p, "/"); print p[1]}')
		totalPages=$((totalPages + pages))
	done

	local pageSize=$(getconf PAGESIZE)

	echo $((totalPages * pageSize / 1024))
}

function assert_greater_than {
	assert_less_than $2 $1
}

saunafs_wait_for_all_ready_chunkservers

file_size=2048
# An acceptable margin of error for the cache size (1 GiB)
minimum_difference=$((1024 * 1024))
sleepAfterDropCaches=5

# Ensure the cache is clean
drop_caches
sleep ${sleepAfterDropCaches}

# Get the original cache size
original_cache_size=$(getSystemCachedMemorySizeKiB)

# Write a relatively big file to populate the cache
echo "Writing a big file to populate the cache"
dd if=/dev/zero of=file bs=1M count=${file_size} oflag=direct &> /dev/null

# Store the cache size after writing a big file (system and used by chunks)
cache_size_with_HDD_ADVISE_NO_CACHE_disabled=$(getSystemCachedMemorySizeKiB)
cache_used_by_chunks_HDD_ADVISE_NO_CACHE_disabled=$(getCachedMemoryUsedByChunksKiB)
echo "Cache used by chunks: ${cache_used_by_chunks_HDD_ADVISE_NO_CACHE_disabled}"

echo "Restarting the chunkserver to enable HDD_ADVISE_NO_CACHE"

## Set HDD_ADVISE_NO_CACHE to 1 and restart the chunkserver
echo "HDD_ADVISE_NO_CACHE = 1" >> "${info[chunkserver0_cfg]}"
saunafs_chunkserver_daemon 0 restart
saunafs_wait_for_all_ready_chunkservers

# Ensure the cache is clean again
drop_caches
sleep ${sleepAfterDropCaches}

# Overwrite the file to re-populate the cache
echo "Overwriting the file to re-populate the cache"
dd if=/dev/zero of=file bs=1M count=${file_size} oflag=direct &> /dev/null

cache_size_with_HDD_ADVISE_NO_CACHE_enabled=$(getSystemCachedMemorySizeKiB)
cache_used_by_chunks_HDD_ADVISE_NO_CACHE_enabled=$(getCachedMemoryUsedByChunksKiB)
echo "Cache used by chunks: ${cache_used_by_chunks_HDD_ADVISE_NO_CACHE_enabled}"

echo "Summary:"
echo "Cache size: ${original_cache_size} kB - original"
echo "Cache size: ${cache_size_with_HDD_ADVISE_NO_CACHE_disabled} kB - HDD_ADVISE_NO_CACHE = 0"
echo "Cache size: ${cache_size_with_HDD_ADVISE_NO_CACHE_enabled} kB - HDD_ADVISE_NO_CACHE = 1"

# Assertions

difference=$((cache_size_with_HDD_ADVISE_NO_CACHE_disabled - original_cache_size))

# Ensure the cache was increased while the HDD_ADVISE_NO_CACHE was disabled
assert_greater_than ${difference} ${minimum_difference}

difference=$((cache_size_with_HDD_ADVISE_NO_CACHE_disabled - cache_size_with_HDD_ADVISE_NO_CACHE_enabled))

# Ensure the cache was decreased while the HDD_ADVISE_NO_CACHE was enabled
assert_greater_than ${difference} ${minimum_difference}

assert_greater_than ${cache_used_by_chunks_HDD_ADVISE_NO_CACHE_disabled} ${minimum_difference}
assert_less_than ${cache_used_by_chunks_HDD_ADVISE_NO_CACHE_enabled} ${minimum_difference}

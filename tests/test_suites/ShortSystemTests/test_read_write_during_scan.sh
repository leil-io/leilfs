timeout_set 2 minutes

# Create an installation with 3 chunkservers, 3 disks each.
# All disks in CS 0 will fail during the test.
if is_windows_system; then
    MOUNT_EXTRA_CONFIG="sfscachemode=NEVER"
else
    MOUNT_EXTRA_CONFIG="sfscachemode=NEVER|sfsdirectio=1"
fi

USE_RAMDISK=YES \
	MOUNTS=1 \
	CHUNKSERVERS=3 \
	DISK_PER_CHUNKSERVER=3
	setup_local_empty_saunafs info

# Create a directory with many files on mountpoint
cd "${info[mount0]}"
mkdir goal3
saunafs setgoal 2 goal3

for file in {1..1000}; do
	FILE_SIZE=1K file-generate goal3/test_${file}
done

saunafs_chunkserver_daemon 0 stop
saunafs_chunkserver_daemon 1 stop
saunafs_chunkserver_daemon 2 stop

saunafs_chunkserver_daemon 0 start
saunafs_chunkserver_daemon 1 start
LD_PRELOAD="${SAUNAFS_INSTALL_FULL_LIBDIR}/libslow_chunk_scan.so" saunafs_chunkserver_daemon 2 start

saunafs_wait_for_all_ready_chunkservers

sleep 5

# if this timeouts then there is a bug
for file in {1..100}; do
	FILE_SIZE=1K file-generate goal3/test_valid_${file}
	file-validate goal3/test_valid_${file}
done

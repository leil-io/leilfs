timeout_set 5 minutes

CHUNKSERVERS=1 \
	MOUNTS=2 \
	MOUNT_0_EXTRA_CONFIG="limitglibcmallocarenas=8" \
	MOUNT_1_EXTRA_CONFIG="limitglibcmallocarenas=2" \
    setup_local_empty_saunafs info

# Get the PIDs of the sfsmount processes
pids=($(pgrep -fa sfsmount | awk '{print $1}'))

# Access the PIDs separately
pid1=${pids[0]}
pid2=${pids[1]}

echo "pids: ${pids[@]}"
echo "pid1: $pid1, pid2: $pid2"

# Function to measure virtual memory usage (VSZ) for a given PID
get_virtual_memory_for_pid() {
    local pid=$1
    ps -o vsz= -p ${pid}
}

# Function to run fio commands and measure VSZ for a given mount point
run_fio_and_measure_vsz() {
    local mount_point=$1
    local pid=$2

    # Run fio write command
    fio --name=test_write --directory=$mount_point --size=128M --rw=write --ioengine=libaio \
        --group_reporting --numjobs=4 --bs=1M --direct=1 --iodepth=1 > /dev/null 2>&1

    # Measure virtual memory usage
    local vsz=$(get_virtual_memory_for_pid $pid)
    echo "$vsz"
}

# Measure VSZ for both mount points
echo "Measuring VSZ for both mount points"
vsz_mount0=$(run_fio_and_measure_vsz "${info[mount0]}" $pid1)
vsz_mount1=$(run_fio_and_measure_vsz "${info[mount1]}" $pid2)

# Print the VSZ values
echo "VSZ for ${info[mount0]} (limitglibcmallocarenas=8): ${vsz_mount0}"
echo "VSZ for ${info[mount1]} (limitglibcmallocarenas=2): ${vsz_mount1}"

# Assert that memory usage is lower for mount1 (limitglibcmallocarenas=2)
assert_less_or_equal "${vsz_mount1}" "${vsz_mount0}"
echo "OK: Memory usage is reduced when limitglibcmallocarenas=2"

cd $TEMP_DIR

saunafs_mount_unmount 0
saunafs_mount_unmount 1

# Use malloctrimperiod option
echo "malloctrimperiod=100" >> "${info[mount0_cfg]}"
echo "malloctrimperiod=1000" >> "${info[mount1_cfg]}"

saunafs_mount_start 0
saunafs_mount_start 1

malloc_trim_job_size=128
malloc_trim_job_num=5

# Let's check writes work, at least 128M per job
fio --name=test_multiple_writes_with_malloc_trim --directory="${info[mount0]}" \
	--size=${malloc_trim_job_size}M --rw=write --ioengine=libaio --group_reporting \
	--numjobs=${malloc_trim_job_num} --bs=1M --direct=1 --iodepth=1 > /dev/null 2>&1
fio --name=test_multiple_writes_with_malloc_trim --directory="${info[mount1]}" \
	--size=${malloc_trim_job_size}M --rw=write --ioengine=libaio --group_reporting \
	--numjobs=${malloc_trim_job_num} --bs=1M --direct=1 --iodepth=1 > /dev/null 2>&1

fio --name=test_multiple_writes_with_malloc_trim --directory="${info[mount0]}" \
	--size=${malloc_trim_job_size}M --rw=read --ioengine=libaio --group_reporting \
	--numjobs=${malloc_trim_job_num} --bs=1M --direct=1 --iodepth=1 > /dev/null 2>&1
fio --name=test_multiple_writes_with_malloc_trim --directory="${info[mount1]}" \
	--size=${malloc_trim_job_size}M --rw=read --ioengine=libaio --group_reporting \
	--numjobs=${malloc_trim_job_num} --bs=1M --direct=1 --iodepth=1 > /dev/null 2>&1

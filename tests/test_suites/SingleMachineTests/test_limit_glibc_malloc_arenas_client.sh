timeout_set 5 minutes

CHUNKSERVERS=1 \
	MOUNTS=2 \
    MOUNT_EXTRA_CONFIG="sfscachemode=NEVER`
			`|cacheexpirationtime=10000`
			`|readcachemaxsizepercentage=1`
			`|sfschunkserverwavereadto=2000`
			`|sfsioretries=50`
			`|readaheadmaxwindowsize=5000`
			`|sfschunkservertotalreadto=8000" \
	MOUNT_0_EXTRA_CONFIG="limitglibcmallocarenas=8" \
	MOUNT_1_EXTRA_CONFIG="limitglibcmallocarenas=2" \
    setup_local_empty_saunafs info

num_jobs=18
five_percent_ram_mb=$(awk '/MemTotal/ {print int($2 / 1024 * 0.05)}' /proc/meminfo)
size_per_job=$(echo "${five_percent_ram_mb} / ${num_jobs}" | bc)
echo "five_percent_ram_mb: ${five_percent_ram_mb}, num_jobs: ${num_jobs}, size_per_job: ${size_per_job}"

pgrep -fa sfsmount
# Get the PIDs of the sfsmount processes
pids=($(pgrep -fa sfsmount | awk '{print $1}'))

# Access the PIDs separately
pid1=${pids[0]}
pid2=${pids[1]}

echo "pids: ${pids[@]}"
echo "pid1: $pid1, pid2: $pid2"

function getVirtualMemoryForPid() {
	pid=${1}
	ps -o vsz= -p ${pid}
}

# Function to run fio commands and measure VSZ for a given mount point
run_fio_and_measure_vsz() {
	# Prepare measure the virtual memory usage during the fio read command
	local mount_point=$1
	local pid=$2
	local output_file="pidstat_output_$pid.txt"

	cd "$mount_point"
	
	fio --name=test_multiple_reads --directory=$mount_point --size=${size_per_job}M --rw=write --ioengine=libaio --group_reporting --numjobs=${num_jobs} --bs=1M --direct=1 --iodepth=1 > /dev/null 2>&1
	
	# Run the fio read command in the background
	fio --name=test_multiple_reads --directory=$mount_point --size=${size_per_job}M --rw=read --ioengine=libaio --group_reporting --numjobs=${num_jobs} --bs=1M --direct=1 --iodepth=1 > /dev/null 2>&1 &
	
	# Store the fio process ID
	fio_pid=$(pgrep -f "fio --name=test_multiple_reads --directory=$mount_point --size=${size_per_job}M --rw=read")

	# Run pidstat in the background to monitor virtual memory usage
	sleep 1
	virtualMemory=$(getVirtualMemoryForPid $pid)

	# Wait for the fio command to complete
	wait $fio_pid

	# Return the virtual memory usage
	echo $virtualMemory
}

# Run the fio commands and measure VSZ for both mount points
echo "Running fio commands and measuring VSZ for both mount points"
vsz_mount0=$(run_fio_and_measure_vsz "${info[mount0]}" $pid1)
vsz_mount1=$(run_fio_and_measure_vsz "${info[mount1]}" $pid2)

# Print the VSZ values
echo "VSZ for ${info[mount0]}: ${vsz_mount0}"
echo "VSZ for ${info[mount1]}: ${vsz_mount1}"

assert_less_or_equal "${vsz_mount1}" "${vsz_mount0}"

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

timeout_set 4 minutes

assert_program_installed pv

USE_RAMDISK=YES \
	CHUNKSERVERS=5 \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER`
			`|readcachemaxsizepercentage=1`
			`|sfsioretries=1`
			`|maxreadaheadrequests=0`
			`|cacheexpirationtime=10000" \
	MASTER_CUSTOM_GOALS="10 ec_4_1: \$ec(4,1)" \
	setup_local_empty_saunafs info

dir="${info[mount0]}/dir"
mkdir "$dir"
saunafs setgoal ec_4_1 "$dir"
FILE_SIZE=${SAUNAFS_CHUNK_SIZE} file-generate "$dir/file"

saunafs fileinfo "$dir/file"

# Delete data and metadata in parts 2, 3 and 4 of the chunk
for i in 2 3 4; do
	csid_data=$(find_first_chunkserver_with_chunks_matching \
		"chunk_ec2_${i}_of_4*${chunk_data_extension}")
	hdd_data=$(get_data_path "${info[chunkserver${csid_data}_hdd]}")
	chunk_data=$(find "$hdd_data" -name "chunk_ec2_${i}_of_4_*${chunk_data_extension}")
	rm "$chunk_data"

	csid_metadata=$(find_first_chunkserver_with_chunks_matching \
		"chunk_ec2_${i}_of_4*${chunk_metadata_extension}")
	hdd_metadata=$(get_metadata_path "${info[chunkserver${csid_metadata}_hdd]}")
	chunk_metadata=$(find "$hdd_metadata" -name "chunk_ec2_${i}_of_4_*${chunk_metadata_extension}")
	rm "$chunk_metadata"
done

# Calculate the number of iterations
# Assuming system memory is in MB and converting 64GB to MB for the max limit
system_memory_mb=$(free -m | awk '/^Mem:/ {print $2}')
file_size_mb=$((${SAUNAFS_CHUNK_SIZE}/(1024*1024)))
max_memory_mb=65536 # 64GB in MB
iterations=$((system_memory_mb / (file_size_mb * 50) + 1))
max_iterations=$((max_memory_mb / (file_size_mb * 50)))

# Limit the iterations for large memory systems
if [ $iterations -gt $max_iterations ]; then
	iterations=$max_iterations
fi

# Read the file in a loop
for ((i=0; i<iterations; i++)); do
	assert_failure pv -B ${SAUNAFS_BLOCK_SIZE} -E -Z ${SAUNAFS_BLOCK_SIZE} -Y ${dir}/file &> /dev/null
done

sleep 10

assert_success dd if=$dir/file of=/dev/null bs=${SAUNAFS_BLOCK_SIZE} count=1

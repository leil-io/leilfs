if is_windows_system; then
	timeout_set $(( (FILE_SIZE_MB / 25) * 5)) minutes
else
	timeout_set $(( (FILE_SIZE_MB / 25) * 3)) minutes
fi

# Have 4 mounts to use flush packet, 4 that are not going to use it and 2 that might use it.

random_seed=$RANDOM
RANDOM=$random_seed
echo "Using random seed for setup: $random_seed"

CHUNKSERVERS=4 \
	MOUNTS=10 \
	MOUNT_USE_WRITE_FLUSH_PACKET=1 \
	MOUNT_0_EXTRA_CONFIG="sfsusewriteflushpacket=0" \
	MOUNT_1_EXTRA_CONFIG="sfsusewriteflushpacket=0" \
	MOUNT_2_EXTRA_CONFIG="sfsusewriteflushpacket=0" \
	MOUNT_3_EXTRA_CONFIG="sfsusewriteflushpacket=0" \
	MOUNT_4_EXTRA_CONFIG="sfsusewriteflushpacket=$((RANDOM % 2))" \
	MOUNT_5_EXTRA_CONFIG="sfsusewriteflushpacket=$((RANDOM % 2))" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER,readbuffersexpirationtime=100,maxreadaheadrequests=1,readworkers=10" \
	MASTER_CUSTOM_GOALS="10 ec_3_1: \$ec(3,1)" \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

# Create an empty file on the SaunaFS filesystem with ec_3_1 goal
file="${info[mount0]}/file"
touch "${file}"
saunafs setgoal ec_3_1 "${file}"

# Create a temporary file with FILE_SIZE_MB megabytes of generated data
tmpf="${RAMDISK_DIR}/tmpf"
FILE_SIZE=${FILE_SIZE_MB}M file-generate "${tmpf}"

# Run 10 tasks, each of them will copy every tenth kilobyte from the temporary file to the file
# on SaunaFS using 5 concurrent writers and a dedicated mountpoint
for i in {0..9}; do
	(
		file="${info[mount${i}]}/file"
		seq $i 10 $((${FILE_SIZE_MB}*1024-1)) | shuf | expect_success xargs -P5 -IXX \
				dd if="${tmpf}" of="${file}" bs=1K count=1 seek=XX skip=XX conv=notrunc 2>/dev/null
	) &
done
wait

# Validate the result
MESSAGE="Data is corrupted after writing" expect_success file-validate "${file}"

# Validate the parity part
csid=$(find_first_chunkserver_with_chunks_matching 'chunk_ec2_1_of_3*')
saunafs_chunkserver_daemon ${csid} stop

MESSAGE="Parity is corrupted after writing" expect_success file-validate "${file}"

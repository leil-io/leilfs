timeout_set 2 minutes

CHUNKSERVERS=4 \
	MOUNTS=4 \
	MOUNT_0_EXTRA_CONFIG="sfscachemode=NEVER`
			`|sfsuseinodebasedwritealgorithm=0" \
	MOUNT_1_EXTRA_CONFIG="sfscachemode=NEVER`
			`|sfsuseinodebasedwritealgorithm=1" \
	MOUNT_2_EXTRA_CONFIG="sfscachemode=NEVER`
			`|sfsuseinodebasedwritealgorithm=0" \
	MOUNT_3_EXTRA_CONFIG="sfscachemode=NEVER`
			`|sfsuseinodebasedwritealgorithm=1" \
	MASTER_CUSTOM_GOALS="10 ec_3_1: \$ec(3,1)" \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

# Create an empty file on the SaunaFS filesystem with ec_3_1 goal
file="${info[mount0]}/file"
touch "${file}"
saunafs setgoal ec_3_1 "${file}"

# Create a temporary file with 16 megabytes of generated data
tmpf="${RAMDISK_DIR}/tmpf"
FILE_SIZE=16M file-generate "${tmpf}"

# Run 4 tasks, each of them will copy every tenth kilobyte from the temporary file to the file
# on SaunaFS using 5 concurrent writers and a dedicated mountpoint
for i in {0..3}; do
	(
		file="${info[mount${i}]}/file"
		seq $i 4 $((16*1024-1)) | shuf | expect_success xargs -P5 -IXX \
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

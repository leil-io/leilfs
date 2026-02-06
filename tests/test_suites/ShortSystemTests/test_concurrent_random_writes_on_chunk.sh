timeout_set 30 seconds

CHUNKSERVERS=8 \
	USE_RAMDISK=YES \
	MOUNTS=4 \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	MASTER_CUSTOM_GOALS="8 ec62: \$ec(6,2)"
	setup_local_empty_saunafs info

cd "${info[mount0]}"

mkdir dir
saunafs setgoal ec62 dir

times_to_repeat=512
FILE_SIZE=$(( times_to_repeat * 4 * 1024 )) file-generate ${TEMP_DIR}/original_file

# Write 4KB at a time, 1KB in each of 4 mounts, and repeat this 512 times, so that the file is
# written in random order and with many concurrent writes.

for i in $(seq 0 $((times_to_repeat - 1))); do
	shuffled_seq=($(shuf -e $(seq 0 3)))
	for mount in $(seq 0 3); do
		dd if="${TEMP_DIR}/original_file" of="${info[mount${mount}]}/dir/file" bs=1K \
			skip=$(( i * 4 + ${shuffled_seq[$mount]} )) \
			seek=$(( i * 4 + ${shuffled_seq[$mount]} )) \
			count=1 conv=notrunc 2>/dev/null &
	done
	wait
	echo "Done writing $i-th block of 4KB"
done

MESSAGE="Validating file after concurrent random writes" expect_success file-validate dir/file

timeout_set 45 seconds

CHUNKSERVERS=8 \
	USE_RAMDISK=YES \
	MOUNTS=4 \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	MASTER_CUSTOM_GOALS="8 ec62: \$ec(6,2)"
	setup_local_empty_saunafs info

cd "${info[mount0]}"

mkdir dir
saunafs setgoal ec62 dir

times_to_repeat=1024
FILE_SIZE=$(( times_to_repeat * 4 * 1024 )) file-generate ${TEMP_DIR}/original_file

# Write 4KB at a time, 1KB in each of 4 mounts, and repeat this 1024 times, so that the file is
# written in random order and with many concurrent writes.

master_reloading_loop_file=${TEMP_DIR}/master_reloading_loop_file
client_tweaking_loop_file=${TEMP_DIR}/client_tweaking_loop_file
switch_use_chunkserver_side_chunk_lock_thread() {
	touch ${master_reloading_loop_file}
	while true; do
		if [ ! -e ${master_reloading_loop_file} ]; then
			break
		fi
		sleep 0.15
		current=$(grep USE_CHUNKSERVER_SIDE_CHUNK_LOCK ${info[master0_cfg]} | tail -n 1 | awk '{print $3}')
		echo "Switching USE_CHUNKSERVER_SIDE_CHUNK_LOCK to $(( 1 - current ))"
		sed -i "s/USE_CHUNKSERVER_SIDE_CHUNK_LOCK = ./USE_CHUNKSERVER_SIDE_CHUNK_LOCK = $(( 1 - current ))/g" ${info[master0_cfg]}
		saunafs_master_daemon reload
	done
	echo "switch_use_chunkserver_side_chunk_lock_thread stopped"
}

switch_use_write_flush_packet_thread() {
	touch ${client_tweaking_loop_file}
	while true; do
		if [ ! -e ${client_tweaking_loop_file} ]; then
			break
		fi
		# Make the test more robust by switching write flush packet usage on the fly on the
		# client side, which can cause more interleaving of operations and increase chances of
		# catching concurrency issues.
		sleep 0.23
		for mount in $(seq 0 3); do
			current=$(cat ${info[mount${mount}]}/.saunafs_tweaks | grep -i UseWriteFlushPacket \
				| awk '{print $2}')
			case ${current} in
				true)  new_value=false ;;
				*)     new_value=true  ;;
			esac
			echo "UseWriteFlushPacket=${new_value}" | sudo tee ${info[mount${mount}]}/.saunafs_tweaks
		done
	done
	echo "switch_write_flush_packet_thread stopped"
}

stop_switch_use_chunkserver_side_chunk_lock_thread() {
	rm -f ${master_reloading_loop_file}
}

stop_switch_use_write_flush_packet_thread() {
	rm -f ${client_tweaking_loop_file}
}

switch_use_chunkserver_side_chunk_lock_thread &
switch_use_chunkserver_side_chunk_lock_thread_pid=$!
switch_use_write_flush_packet_thread &
switch_use_write_flush_packet_thread_pid=$!

for i in $(seq 0 $((times_to_repeat - 1))); do
	shuffled_seq=($(shuf -e $(seq 0 3)))
	pids=()
	for mount in $(seq 0 3); do
		dd if="${TEMP_DIR}/original_file" of="${info[mount${mount}]}/dir/file" bs=1K \
			skip=$(( i * 4 + ${shuffled_seq[$mount]} )) \
			seek=$(( i * 4 + ${shuffled_seq[$mount]} )) \
			count=1 conv=notrunc 2>/dev/null &
		pids+=("$!")
	done
	if [ ${#pids[@]} -gt 0 ]; then
		wait "${pids[@]}"
	fi
	echo "Done writing $i-th block of 4KB"
done

stop_switch_use_chunkserver_side_chunk_lock_thread
stop_switch_use_write_flush_packet_thread

MESSAGE="Validating file after concurrent random writes" expect_success file-validate dir/file

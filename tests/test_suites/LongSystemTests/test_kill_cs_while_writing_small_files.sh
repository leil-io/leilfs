timeout_set 25 minutes

CHUNKSERVERS=8 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER,sfswriteworkers=30,sfsioretries=13" \
	CHUNKSERVER_EXTRA_CONFIG="NR_OF_NETWORK_WORKERS = 1|NR_OF_HDD_WORKERS_PER_NETWORK_WORKER = 1" \
	MASTER_EXTRA_CONFIG="CHUNKS_WRITE_REP_LIMIT = 10|CHUNKS_READ_REP_LIMIT = 10|`
		`CHUNKS_LOOP_MIN_TIME = 1|CHUNKS_LOOP_MAX_CPU = 90|CHUNKS_LOOP_PERIOD = 50|`
		`OPERATIONS_DELAY_INIT = 0" \
	MASTER_CUSTOM_GOALS="8 ec62: \$ec(6,2)"
	setup_local_empty_saunafs info

cd ${info[mount0]}

number_of_files=1000

for i in $(seq 1 ${number_of_files}); do
	dd if=/dev/random of=${TEMP_DIR}/file_$i bs=64K count=6 conv=fsync &> /dev/null
done

mkdir dir
saunafs setgoal ec62 dir
saunafs settrashtime 0 dir

for test_loop in {1..10}; do
	for i in $(seq 1 ${number_of_files}); do
		dd if="${TEMP_DIR}/file_${i}" of="dir/file_${i}" bs=384K count=1 status=none &> /dev/null &
	done

	# Write some files with all CSs available, then kill some CSs and write more files, to increase
	# the probability of having chunks with parts being written at a time when the chunkservers are
	# killed.
	sleep 0.3

	saunafs_chunkserver_daemon 0 kill
	saunafs_chunkserver_daemon 1 kill
	echo "Chunkservers killed, now starting them again"

	wait
	echo "All files written"

	# Wait enough time for the chunks to have increased their version because of the lost copies,
	# which will prevent the chunks from having copies with wrong data.
	sleep 10
	echo "Starting chunkservers"

	saunafs_chunkserver_daemon 0 start
	saunafs_chunkserver_daemon 1 start
	saunafs_wait_for_all_ready_chunkservers

	# Wait for the chunkservers to fix the chunks, which may take some time because of the number of
	# missing parts and the fact that we have only 1 worker thread per chunkserver.
	while true; do
		current_chunk_ok=0
		for i in $(seq 1 ${number_of_files}); do
			if saunafs fileinfo dir/file_$i | tail -n1 | grep -E \
				'^[[:space:]]*copy 8:' > /dev/null; then
				current_chunk_ok=$((current_chunk_ok + 1))
			fi
		done

		echo "Checked all files, ${current_chunk_ok} copies are OK"
		if (( current_chunk_ok == number_of_files )); then
			break
		fi

		sleep 5
	done

	saunafs_chunkserver_daemon 6 stop
	saunafs_chunkserver_daemon 7 stop

	# Test if we can read files with only 6 copies, specially when reading from chunkservers 0 and
	# 1, which were killed and restarted.
	for i in $(seq 1 ${number_of_files}); do
		assert_success dd if="dir/file_$i" of=/dev/null bs=384K count=1 status=none
		cmp "${TEMP_DIR}/file_$i" "dir/file_$i" || \
			{ echo "File $i is different after reading back"; exit 1; }
	done

	saunafs_chunkserver_daemon 6 start
	saunafs_chunkserver_daemon 7 start
	saunafs_wait_for_all_ready_chunkservers

	rm dir/file_*

	# Ensure all CS report zero chunks before starting the next loop, to reuse the space
	while true; do
		cs_with_chunks=$(saunafs-admin list-chunkservers localhost "${info[matocl]}" | \
			awk '/chunks:/ && $2!=0 {print $0}' | wc -l)

		echo "${cs_with_chunks} CSs report non-zero chunk counts"
		if (( cs_with_chunks == 0 )); then
			break
		fi

		sleep 1
	done

	echo "Test loop ${test_loop} completed"
done

timeout_set 1 minute

CHUNKSERVERS=8 \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER,sfswriteworkers=100,sfsioretries=13" \
	CHUNKSERVER_EXTRA_CONFIG="NR_OF_NETWORK_WORKERS = 1|NR_OF_HDD_WORKERS_PER_NETWORK_WORKER = 1|`
		`BGJOBSCNT_PER_NETWORK_WORKER = 10" \
	MASTER_CUSTOM_GOALS="8 ec62: \$ec(6,2)"
	setup_local_empty_saunafs info

cd ${info[mount0]}

number_of_files=500

for i in $(seq 1 ${number_of_files}); do
	dd if=/dev/random of=${TEMP_DIR}/file_$i bs=64K count=6 conv=fsync &> /dev/null
done

mkdir dir
saunafs setgoal ec62 dir
saunafs settrashtime 0 dir

for i in $(seq 1 ${number_of_files}); do
	(assert_success dd if="${TEMP_DIR}/file_${i}" of="dir/file_${i}" bs=384K count=1 \
		status=none &> /dev/null) &
done

wait
echo "All files written"

saunafs_chunkserver_daemon 0 stop
saunafs_chunkserver_daemon 1 stop

for i in $(seq 1 ${number_of_files}); do
	assert_success dd if="dir/file_$i" of=/dev/null bs=384K count=1 status=none
	cmp "${TEMP_DIR}/file_$i" "dir/file_$i" || \
		{ echo "File $i is different after reading back"; exit 1; }
done

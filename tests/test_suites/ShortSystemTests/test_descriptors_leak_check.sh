timeout_set 1 minute

CHUNKSERVERS=1 \
	DISK_PER_CHUNKSERVER=1 \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

max_files=100
max_open_descriptors=10
time_limit=45
for ((files_created=0; files_created < max_files; ++files_created)); do
	tmp_file=$(mktemp -p ${info[mount0]})
	dd if=/dev/zero of=$tmp_file bs=33 count=1000 2> /dev/null &
done

wait

firstDisk=$(head -1 "${info[chunkserver0_hdd]}")
isZoned=$(is_zoned_device "${firstDisk}")

# wait for saunafs to close files
cs_pid=$(saunafs_chunkserver_daemon 0 test | sed 's/.*: //')
for ((time_elapsed=0; time_elapsed < time_limit; ++time_elapsed)); do
	leaked_descriptors_number=$(lsof +D $RAMDISK_DIR -p$cs_pid 2>/dev/null | \
		grep -v 'lock' | grep chunk_ | wc -l)

	if $isZoned; then
		open_zones=$(lsof +D "/mnt/zoned/sauna_nullb0" -p$cs_pid 2>/dev/null | \
			grep -v 'lock' | grep "/zoned/" | wc -l)
		echo "Open zones: $open_zones"
		leaked_descriptors_number=$((leaked_descriptors_number + open_zones))
	fi

	if ((leaked_descriptors_number < max_open_descriptors)); then
		break
	fi

	if ! ((time_elapsed % 10)); then
		echo Open descriptors: $((leaked_descriptors_number))
	fi

	sleep 1
done

echo Open descriptors: $((leaked_descriptors_number))

if ((leaked_descriptors_number >= max_open_descriptors)); then
	test_add_failure "$leaked_descriptors_number files are not closed"
fi


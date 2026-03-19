timeout_set 3 minutes
assert_program_installed fio

io_priority_mode=${IO_PRIORITY_MODE:-FIFO}

if is_windows_system; then
	io_engine=sync
else
	io_engine=libaio
fi

CHUNKSERVERS=4 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="cacheexpirationtime=0" \
	CHUNKSERVER_EXTRA_CONFIG="IO_PRIORITY_MODE=${io_priority_mode}"
setup_local_empty_saunafs info

cd ${info[mount0]}

mkdir dir_std

for count in {1..2}; do
	touch dir_std/file
	FILE_SIZE=100000000 BLOCK_SIZE=2135 file-generate dir_std/file &
	file-validate-growing dir_std/file 100000000
	rm dir_std/file
done

assert_success fio --directory="${info[mount0]}/dir_std" --name=simultaneous_rw --rw=rw --bs=64K \
	--size=400M --numjobs=4 --direct=1 --group_reporting --ioengine=${io_engine}

timeout_set 3 minutes

CHUNKSERVERS=1 \
	USE_RAMDISK=YES \
	CHUNKSERVER_EXTRA_CONFIG="READ_AHEAD_KB = 1024|MAX_READ_BEHIND_KB = 2048" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER,sfsioretries=10" \
	setup_local_empty_saunafs info

test_error_cleanup() {
	sudo umount -l "${TEMP_DIR}/mnt/ganesha"
	sudo pkill -9 ganesha.nfsd
}

mkdir -p "${TEMP_DIR}/mnt/ganesha"

create_ganesha_pid_file

cd ${info[mount0]}

cat <<EOF > ${info[mount0]}/ganesha.conf
NFSV4 {
	Grace_Period = 10;
	Lease_Lifetime = 10;
}
EXPORT {
	Attr_Expiration_Time = 10;
	Export_Id = 2;
	Path = /;
	Pseudo = /;
	Access_Type = RW;
	FSAL {
		Name = SaunaFS;
		hostname = localhost;
		port = ${saunafs_info_[matocl]};
		# How often to retry to connect
		io_retries = 15;
		cache_expiration_time_ms = 2500;
	}
	Protocols = 4;
	CLIENT {
		Clients = localhost;
	}
}
EOF

sudo /usr/bin/ganesha.nfsd -f ${info[mount0]}/ganesha.conf -L /tmp/ganesha.log

check_rpc_service
sudo mount -vvvv localhost:/ "${TEMP_DIR}/mnt/ganesha"

# Write a big file to deplete the space of 2 GiB
assert_failure dd if=/dev/zero of="${TEMP_DIR}/mnt/ganesha/largefile" bs=1M count=2048 \
	oflag=direct conv=notrunc status=none

# Create two 1MiB files, which should fail
assert_failure dd if=/dev/zero of="${TEMP_DIR}/mnt/ganesha/1MiBfile.1" bs=1M count=1
assert_failure dd if=/dev/zero of="${TEMP_DIR}/mnt/ganesha/1MiBfile.2" bs=1M count=1

# Create a folder, which should be successful
assert_success mkdir "${TEMP_DIR}/mnt/ganesha/empty.folder"

# Validate empty files were created
assert_success ls -lh "${TEMP_DIR}/mnt/ganesha/1MiBfile.1"
assert_success ls -lh "${TEMP_DIR}/mnt/ganesha/1MiBfile.2"

ls -lh "${TEMP_DIR}/mnt/ganesha/"

test_error_cleanup || true

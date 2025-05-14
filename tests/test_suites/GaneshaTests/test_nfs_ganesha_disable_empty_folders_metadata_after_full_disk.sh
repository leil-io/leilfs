timeout_set 2 minutes

CHUNKSERVERS=1 \
	USE_RAMDISK=YES \
	MASTER_EXTRA_CONFIG="CREATE_EMPTY_FOLDERS_WHEN_SPACE_DEPLETED = 0" \
	CHUNKSERVER_EXTRA_CONFIG="READ_AHEAD_KB = 1024|MAX_READ_BEHIND_KB = 2048`
			`|HDD_LEAVE_SPACE_DEFAULT = 0MiB" \
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

sudo /usr/bin/ganesha.nfsd -f ${info[mount0]}/ganesha.conf

check_rpc_service
sudo mount -vvvv localhost:/ "${TEMP_DIR}/mnt/ganesha"

# Write a 2G file, remaining 48 MiB of available space
assert_success dd if=/dev/zero of="${TEMP_DIR}/mnt/ganesha/largefile" bs=1M count=2000 \
	oflag=direct conv=notrunc status=none

# Create a 48 MiB file to deplete the available space
assert_failure dd if=/dev/zero of="${TEMP_DIR}/mnt/ganesha/48MiBfile" bs=1M count=48 \
	oflag=direct status=none

# Try to create a folder, which should fail
assert_failure mkdir "${TEMP_DIR}/mnt/ganesha/empty.folder"

# Try to create a 1 MiB file, which should fail
assert_failure dd if=/dev/zero of="${TEMP_DIR}/mnt/ganesha/1MiBfile" bs=1M count=1 \
	oflag=direct status=none

# Validate empty file was created
assert_success ls -lh "${TEMP_DIR}/mnt/ganesha/1MiBfile"

# Validate empty folder was not created
assert_failure ls -lh "${TEMP_DIR}/mnt/ganesha/empty.folder"

assert_success ls -lh "${TEMP_DIR}/mnt/ganesha"

test_error_cleanup || true

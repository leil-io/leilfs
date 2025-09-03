#
# To run this test you need to add the following lines to /etc/sudoers.d/saunafstest:
#
# saunafstest ALL = NOPASSWD: /bin/mount, /bin/umount, /bin/pkill, /bin/mkdir, /bin/touch
# saunafstest ALL = NOPASSWD: /usr/bin/ganesha.nfsd
#
# The path for the Ganesha daemon should match the installation folder inside the test.
#

timeout_set 2 minutes

CHUNKSERVERS=3 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER,sfsioretries=10" \
	CHUNKSERVER_EXTRA_CONFIG="READ_AHEAD_KB = 1024|MAX_READ_BEHIND_KB = 2048"
	setup_local_empty_saunafs info

test_error_cleanup() {
	sudo umount -l ${TEMP_DIR}/mnt/ganesha
	sudo pkill -9 ganesha.nfsd
}

mkdir -p ${TEMP_DIR}/mnt/ganesha

create_ganesha_pid_file

cd ${info[mount0]}

cat <<EOF > ${info[mount0]}/ganesha.conf
NFSV4 {
	Grace_Period = 5;
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
		io_retries = 10;
		cache_expiration_time_ms = 2500;
	}
	Protocols = 4;
	CLIENT {
		Clients = localhost;
	}
}
EOF

# This test logs the output to a file to avoid verbose output
sudo /usr/bin/ganesha.nfsd -f ${info[mount0]}/ganesha.conf -L /tmp/ganesha.log

check_rpc_service
sudo mount -vvvv localhost:/ $TEMP_DIR/mnt/ganesha

# Create a file larger than the RAMDISK available space (2G) and upload it
assert_failure dd if=/dev/urandom of=$TEMP_DIR/mnt/ganesha/largefile bs=1M count=3072

ls -lh $TEMP_DIR/mnt/ganesha/largefile
test_error_cleanup || true

#
# To run this test you need to add the following lines to /etc/sudoers.d/saunafstest:
#
# saunafstest ALL = NOPASSWD: /bin/mount, /bin/umount, /bin/pkill, /bin/mkdir, /bin/touch
# saunafstest ALL = NOPASSWD: /usr/bin/ganesha.nfsd
#
# The path for the Ganesha daemon should match the installation folder inside the test.
#

timeout_set 3 minutes

CHUNKSERVERS=5 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	CHUNKSERVER_EXTRA_CONFIG="READ_AHEAD_KB = 1024|MAX_READ_BEHIND_KB = 2048"
	setup_local_empty_saunafs info

test_error_cleanup() {
	cd ${TEMP_DIR}
	sudo umount -l ${TEMP_DIR}/mnt/ganesha
	sudo pkill -9 ganesha.nfsd
}

mkdir -p ${TEMP_DIR}/mnt/ganesha
mkdir -p ${info[mount0]}/data

create_ganesha_pid_file

cd ${info[mount0]}

cat <<EOF > ${info[mount0]}/ganesha.conf
NFSV4 {
	Grace_Period = 5;
}
EXPORT
{
	Attr_Expiration_Time = 0;
	Export_Id = 99;
	Path = /data;
	Pseudo = /data;
	Access_Type = RW;
	FSAL {
		Name = SaunaFS;
		hostname = localhost;
		port = ${saunafs_info_[matocl]};
		# How often to retry to connect
		io_retries = 5;
		cache_expiration_time_ms = 2500;
	}
	Protocols = 4;
	CLIENT {
		Clients = localhost;
	}
}
SaunaFS {
	PNFS_DS = true;
	PNFS_MDS = true;
}
EOF

sudo /usr/bin/ganesha.nfsd -f ${info[mount0]}/ganesha.conf

check_rpc_service
sudo mount -vvvv localhost:/data $TEMP_DIR/mnt/ganesha

mkdir ${TEMP_DIR}/mnt/ganesha/cthon_tests
export NFSTESTDIR="${TEMP_DIR}/mnt/ganesha/cthon_tests"

# Run connectathon nfs suite
cd ${TEMP_DIR}

git clone https://github.com/leil-io/cthon04.git
cd cthon04
make all

./runtests -b -n
./runtests -l -n
./runtests -s -n

test_error_cleanup || true

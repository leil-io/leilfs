#
# To run this test you need to add the following lines to /etc/sudoers.d/saunafstest:
#
# saunafstest ALL = NOPASSWD: /bin/mount, /bin/umount, /bin/pkill, /bin/mkdir, /bin/touch
# saunafstest ALL = NOPASSWD: /usr/bin/ganesha.nfsd
#
# The path for the Ganesha daemon should match the installation folder inside the test.
#
# Goal of test:
# Verify the performance of Ganesha and Linux clients when performing write
# with dd tool.
#

timeout_set 2 minutes

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
mkdir -p ${info[mount0]}/linux

create_ganesha_pid_file

cd ${info[mount0]}

cat <<EOF > ${info[mount0]}/ganesha.conf
NFSV4 {
	Grace_Period = 5;
}
EXPORT
{
	Attr_Expiration_Time = 0;
	Export_Id = 1;
	Path = /;
	Pseudo = /;
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
sudo mount -vvvv localhost:/ $TEMP_DIR/mnt/ganesha

echo ""
echo "Running dd write on top of Ganesha Client:"

# Run dd write on top of Ganesha Client
ganesha_report="$(dd if=/dev/random bs=500M count=1 \
                  of=${TEMP_DIR}/mnt/ganesha/file.dd status=progress 2>&1)"

# print Ganesha write statistics
echo "${ganesha_report}"

ganesha_write_speed="$(echo "${ganesha_report}" | tail -1 | rev | \
                       cut -d' ' -f1,2 | rev)"

echo ""
echo "Running dd write on top of Linux Client:"

# Run dd write on top of Linux Client
linuxClient_report="$(dd if=/dev/random bs=500M count=1 \
                      of=${info[mount0]}/linux/file.dd status=progress 2>&1)"

# print Linux Client write statistics
echo "${linuxClient_report}"

linuxClient_write_speed="$(echo "${linuxClient_report}" | tail -1 | rev | \
                           cut -d' ' -f1,2 | rev)"

# Show performances of both clients
echo ""
echo "==============================================="
echo " Operation: Ganesha Client --- Linux Client ---"
echo -e " WRITE: \t ${ganesha_write_speed} \t ${linuxClient_write_speed}"
echo "==============================================="

test_error_cleanup || true

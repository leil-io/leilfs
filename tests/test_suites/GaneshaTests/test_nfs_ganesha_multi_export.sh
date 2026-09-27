#
# To run this test you need to add the following lines to /etc/sudoers.d/saunafstest:
#
# saunafstest ALL = NOPASSWD: /bin/mount, /bin/umount, /bin/pkill, /bin/mkdir, /bin/touch
# saunafstest ALL = NOPASSWD: /usr/bin/ganesha.nfsd
#
# The path for the Ganesha daemon should match the installation folder inside the test.
#

timeout_set 2 minutes

CHUNKSERVERS=5 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	CHUNKSERVER_EXTRA_CONFIG="READ_AHEAD_KB = 1024|MAX_READ_BEHIND_KB = 2048"
	setup_local_empty_saunafs info

ganesha_config="${info[mount0]}/ganesha.conf"
ganesha_log="${TEMP_DIR}/ganesha-multi-export.log"

cleanup_multi_export() {
	for x in 1 2 97 99; do
		local mountpoint_path="${TEMP_DIR}/mnt/nfs${x}"
		if mountpoint -q "${mountpoint_path}"; then
			sudo umount -l "${mountpoint_path}" || true
		fi
	done
	sudo pkill -9 ganesha.nfsd 2>/dev/null || true
}

test_error_cleanup() {
	if [[ -f ${ganesha_log} ]]; then
		echo "===== Ganesha multi-export log (last 200 lines) ====="
		tail -n 200 "${ganesha_log}" || true
	fi
	cleanup_multi_export
}
trap test_error_cleanup EXIT

cd ${info[mount0]}

# Create mountpoints for testing
mkdir $TEMP_DIR/mnt/nfs{1,2,97,99}
mkdir ganesha

create_ganesha_pid_file

cat <<EOF > "${ganesha_config}"
EXPORT
{
	Attr_Expiration_Time = 0;
	Export_Id = 1;
	Path = /export1;
	Pseudo = /e1;
	Access_Type = RW;
	FSAL {
		Name = SaunaFS;
		hostname = localhost;
		port = ${saunafs_info_[matocl]};
	}
	Protocols = 3, 4;
}
EXPORT
{
	Attr_Expiration_Time = 0;
	Export_Id = 2;
	Path = /export2;
	Pseudo = /e2;
	Access_Type = RW;
	FSAL {
		Name = SaunaFS;
		hostname = localhost;
		port = ${saunafs_info_[matocl]};
	}
	Protocols = 3, 4;
}
EXPORT
{
	Attr_Expiration_Time = 0;
	Export_Id = 97;
	Path = /;
	Pseudo = /e97;
	Access_Type = MDONLY;
	FSAL {
		Name = SaunaFS;
		hostname = localhost;
		port = ${saunafs_info_[matocl]};
	}
	Protocols = 4;
}
EXPORT
{
	Attr_Expiration_Time = 0;
	Export_Id = 99;
	Path = /;
	Pseudo = /e99;
	Access_Type = RO;
	FSAL {
		Name = SaunaFS;
		hostname = localhost;
		port = ${saunafs_info_[matocl]};
	}
	Protocols = 4;
}
SaunaFS {
	PNFS_DS = true;
	PNFS_MDS = true;
}
NFSV4 {
	Grace_Period = 5;
}
EOF

mkdir ${info[mount0]}/export{1,2}

touch ${info[mount0]}/export1/test1
touch ${info[mount0]}/export2/test2

echo "Starting Ganesha with four exports"
sudo /usr/bin/ganesha.nfsd -f "${ganesha_config}" -L "${ganesha_log}"

check_rpc_service

echo "Mounting Ganesha exports"
for x in 1 2 99; do
	echo "Mounting NFSv4.1 export /e${x}"
	sudo mount -o v4.1 localhost:/e${x} ${TEMP_DIR}/mnt/nfs${x}
done
echo "Mounting NFSv4 export /e97"
sudo mount -o nfsvers=4 localhost:/e97 $TEMP_DIR/mnt/nfs97

echo "Checking export visibility"
find $TEMP_DIR/mnt/nfs1 * | grep test1
assert_empty "$(find $TEMP_DIR/mnt/nfs1 | grep test2 | cat)"
find $TEMP_DIR/mnt/nfs2 * | grep test2
assert_empty "$(find $TEMP_DIR/mnt/nfs2 | grep test1 | cat)"

ls -l $TEMP_DIR/mnt/nfs1
ls -l $TEMP_DIR/mnt/nfs2
ls -l $TEMP_DIR/mnt/nfs97
ls -l $TEMP_DIR/mnt/nfs99

echo "Generating files through writable exports"
FILE_SIZE=1234567 file-generate $TEMP_DIR/mnt/nfs1/test1.bin
FILE_SIZE=2345678 file-generate $TEMP_DIR/mnt/nfs2/test2.bin

echo "Validating files through writable and read-only exports"
file-validate $TEMP_DIR/mnt/nfs1/test1.bin
file-validate $TEMP_DIR/mnt/nfs2/test2.bin
file-validate $TEMP_DIR/mnt/nfs99/export1/test1.bin
file-validate $TEMP_DIR/mnt/nfs99/export2/test2.bin

# Files on export97 are "metadata only", so file validation should fail
assert_failure file-validate $TEMP_DIR/mnt/nfs97/export1/test1.bin
assert_failure file-validate $TEMP_DIR/mnt/nfs97/export2/test2.bin

echo "Cleaning up Ganesha multi-export test"
cleanup_multi_export
trap - EXIT

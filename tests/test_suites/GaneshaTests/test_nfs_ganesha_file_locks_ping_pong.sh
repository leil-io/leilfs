timeout_set 3 minutes

CHUNKSERVERS=5 \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

test_error_cleanup() {
	cd ${TEMP_DIR}
	for x in "${nfs_mounts[@]}"; do
		sudo umount -l "${TEMP_DIR}/mnt/nfs${x}"
	done
	sudo pkill -9 ganesha.nfsd
}

create_ganesha_pid_file

# Create mountpoints for testing
mkdir "${TEMP_DIR}/mnt/nfs"{2,3,5,7,11}

cat <<EOF > "${TEMP_DIR}/ganesha.conf"
NFSV4 {
	Grace_Period = 5;
	Lease_Lifetime = 5;
}
EOF

for id in 2 3 5 7 11; do
	cat <<EOF_EXPORT >> "${TEMP_DIR}/ganesha.conf"
EXPORT
{
	Attr_Expiration_Time = 10;
	Export_Id = ${id};
	Path = /;
	Pseudo = /export${id};
	Access_Type = RW;
	FSAL {
		Name = SaunaFS;
		hostname = localhost;
		port = ${saunafs_info_[matocl]};
	}
	Protocols = 4;
}
EOF_EXPORT
done

sudo /usr/bin/ganesha.nfsd -f "${TEMP_DIR}/ganesha.conf" -L /tmp/ganesha.log

check_rpc_service

nfs_mounts=(2 3 5 7 11)

for x in "${nfs_mounts[@]}"; do
	sudo mount localhost:/export${x} "${TEMP_DIR}/mnt/nfs${x}"
done

touch "${TEMP_DIR}/mnt/nfs2/lockfile"

declare -a ping_pongs

# Launch ping pong instances
for x in "${nfs_mounts[@]}"; do
	cd "${TEMP_DIR}/mnt/nfs${x}"
	safs_ping_pong -rw lockfile 6&
	ping_pongs[$x]=$!
done

# Ensure that all ping pong tests finished with no errors
for x in "${nfs_mounts[@]}"; do
	wait ${ping_pongs[$x]}
	assert_equals 0 $?
done

test_error_cleanup

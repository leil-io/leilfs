assert_program_installed dbench

timeout_set 3 minutes

CHUNKSERVERS=3 \
	setup_local_empty_saunafs info

test_error_cleanup() {
	cd ${TEMP_DIR}
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
EXPORT
{
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
sudo mount -vvvv localhost:/ ${TEMP_DIR}/mnt/ganesha

cd ${TEMP_DIR}/mnt/ganesha

dbench -s -S -t 60 1
if [[ ${PIPESTATUS[0]} != 0 ]]; then
	test_fail "dbench failed"
fi

test_error_cleanup || true

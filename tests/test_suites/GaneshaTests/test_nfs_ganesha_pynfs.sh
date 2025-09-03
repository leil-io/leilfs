timeout_set 5 minutes

CHUNKSERVERS=3 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	CHUNKSERVER_EXTRA_CONFIG="READ_AHEAD_KB = 1024|MAX_READ_BEHIND_KB = 2048"
	setup_local_empty_saunafs info

create_ganesha_pid_file

cd ${info[mount0]}

cat <<EOF > ${info[mount0]}/ganesha.conf
NFSV4 {
	Grace_Period = 5;
	Lease_Lifetime = 5;
}
EXPORT
{
	Attr_Expiration_Time = 0;
	Export_Id = 99;
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
EOF

sudo /usr/bin/ganesha.nfsd -f ${info[mount0]}/ganesha.conf

check_rpc_service

# Run pynfs suite
cd ${TEMP_DIR}

git clone git://linux-nfs.org/~bfields/pynfs.git
cd pynfs && yes | python3 setup.py build > /${TEMP_DIR}/output_tempfile.txt
echo $?

cd ${TEMP_DIR}/pynfs/nfs4.1

# Testing export localhost:/ for NFS v4.1
./testserver.py localhost:/ --verbose --maketree --showomit --rundeps all ganesha

sudo pkill -9 ganesha.nfsd

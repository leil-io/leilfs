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
sudo mount -vvvv localhost:/ ${TEMP_DIR}/mnt/ganesha

cd ${TEMP_DIR}

# Clone the small file test repo
git clone https://github.com/distributed-system-analysis/smallfile.git
cd smallfile

# Run the small file test for each operation
operations="create read chmod stat append rename delete-renamed mkdir rmdir"

for operation in ${operations}; do
	./smallfile_cli.py --files 200 --threads 20 --file-size 64 --hash-into-dirs Y \
	--top ${TEMP_DIR}/mnt/ganesha --operation ${operation}
done

test_error_cleanup || true

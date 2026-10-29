timeout_set 5 minutes

# pynfs needs xdrlib3 or the stdlib xdrlib, which Python 3.13 removed
# (Debian/Ubuntu: python3-standard-xdrlib, or: pip install xdrlib3).
if ! python3 -c "import xdrlib3" 2>/dev/null && ! python3 -c "import xdrlib" 2>/dev/null; then
	test_fail "pynfs needs the xdrlib3 or xdrlib Python module"
fi

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

# Pinned pynfs release, bumped deliberately (list tags: git ls-remote --tags <url>)
pynfs_tag=pynfs-0.5
git clone --depth 1 --branch "${pynfs_tag}" git://git.linux-nfs.org/projects/cdmackay/pynfs.git

# pynfs' top-level setup.py ignores sub-build failures (os.system), so build the
# components testserver.py needs one by one and stop on the first error.
for dir in xdr rpc nfs4.1; do
	if ! (cd ${TEMP_DIR}/pynfs/${dir} && yes | python3 setup.py build) \
			>> ${TEMP_DIR}/pynfs_build.log 2>&1; then
		cat ${TEMP_DIR}/pynfs_build.log
		test_fail "pynfs build failed in ${dir}"
	fi
done

cd ${TEMP_DIR}/pynfs/nfs4.1

# Testing export localhost:/ for NFS v4.1
./testserver.py localhost:/ --verbose --maketree --showomit --rundeps all ganesha

sudo pkill -9 ganesha.nfsd

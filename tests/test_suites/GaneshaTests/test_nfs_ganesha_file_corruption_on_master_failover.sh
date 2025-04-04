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
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	CHUNKSERVER_EXTRA_CONFIG="READ_AHEAD_KB = 1024|MAX_READ_BEHIND_KB = 2048"
	setup_local_empty_saunafs info

grace_period=15
delta=5

test_error_cleanup() {
	cd "${TEMP_DIR}"
	sudo umount -l "${TEMP_DIR}/mnt/ganesha"
	sudo pkill -9 ganesha.nfsd
}

# Function to get the checksum of a given file
get_checksum() {
	sha256sum $1 | awk '{ print $1 }'
}

# Function to get the checksum of a given file, using direct I/O
get_checksum_with_direct_io() {
	dd if="$1" iflag=direct bs=1M 2>/dev/null | sha256sum | awk '{ print $1 }'
}

mkdir -p "${TEMP_DIR}/mnt/ganesha"

create_ganesha_pid_file

cd "${info[mount0]}"

cat <<EOF > "${info[mount0]}/ganesha.conf"
NFSV4 {
	Grace_Period = ${grace_period};
	Lease_Lifetime = ${grace_period};
}
EXPORT {
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
		io_retries = 30;
		cache_expiration_time_ms = 2500;
	}
	Protocols = 4;
	CLIENT {
		Clients = localhost;
	}
}
EOF

# Create a file for testing with checksum
head -c 100M /dev/urandom | tee "${TEMP_DIR}/test_file" > /dev/null

sudo /usr/bin/ganesha.nfsd -f "${info[mount0]}/ganesha.conf"

check_rpc_service
sudo mount -vvvv localhost:/ "${TEMP_DIR}/mnt/ganesha"

# Restart master server after 15 seconds
(
	sleep 15
	assert_success saunafs_master_daemon restart
) &

# Wait for Grace period so NFS Ganesha server will be ready
sleep $((grace_period + delta))

# Copy the file after master restart
cp "${TEMP_DIR}/test_file" "$TEMP_DIR/mnt/ganesha/test_file"

# Get checksums
checksum1=$(get_checksum "${TEMP_DIR}/test_file")
checksum2=$(get_checksum_with_direct_io "${TEMP_DIR}/mnt/ganesha/test_file")
checksum3=$(get_checksum "${info[mount0]}/test_file")

# Print checksums
echo "Checksum of original file: ${checksum1}"
echo "Checksum of file from NFS mount: ${checksum2}"
echo "Checksum of file from SaunaFS mount: ${checksum3}"

# Verify checksums
assert_equals "${checksum1}" "${checksum2}"
assert_equals "${checksum1}" "${checksum3}"

test_error_cleanup || true

timeout_set 1 minute

USE_RAMDISK=YES \
	setup_local_empty_saunafs info

test_error_cleanup() {
	cd ${TEMP_DIR}
	# Kill any remaining posixlockcmd processes
	pkill -f posixlockcmd 2>/dev/null || true
	sudo umount -l ${TEMP_DIR}/mnt/ganesha
	sudo pkill -9 ganesha.nfsd
}

create_ganesha_pid_file

mkdir -p ${TEMP_DIR}/mnt/ganesha

cat <<EOF > ${TEMP_DIR}/ganesha.conf
NFSV4 {
	Grace_Period = 5;
	Lease_Lifetime = 5;
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
	}
	Protocols = 4;
}
EOF

sudo /usr/bin/ganesha.nfsd -f "${TEMP_DIR}/ganesha.conf"

check_rpc_service
sudo mount -vvvv localhost:/ "${TEMP_DIR}/mnt/ganesha"

mkdir "${TEMP_DIR}/mnt/ganesha/dir"
FILE_SIZE="100M" assert_success file-generate "${TEMP_DIR}/mnt/ganesha/dir/file_100M"

function readlock() {
	posixlockcmd $1 r $2 $3 >> "${TEMP_DIR}/posixlock.log" &
}

function writelock() {
	posixlockcmd $1 w $2 $3 >> "${TEMP_DIR}/posixlock.log" &
}

function unlock() {
	kill -s SIGUSR1 $1
}

# helper to assert master-level lock state (order-independent)
#
# manage-locks <master ip> <master port> [list/unlock] [flock/posix/all]
locks_active_count() {
	saunafs_admin_master manage-locks list posix --porcelain --active | wc -l
}

assert_active_eq() {
	local expected="$1"
	local actual=$(($(locks_active_count) - 1))  # Subtract header line
	if [ "$actual" -eq "$expected" ]; then
		echo "✓ Active locks: $actual (expected: $expected)"
		return 0
	else
		echo "✗ Expected $expected active locks, got $actual"
		test_error_cleanup
		return 1
	fi
}

declare -a readlocks
declare -a writelocks

# Go to the Ganesha mount point
cd "${TEMP_DIR}/mnt/ganesha"

echo "Acquire a shared lock on the range [0, 100] in dir/file_100M"
readlock "dir/file_100M" 0 100
readlocks[1]=$!

echo "Acquire an exclusive lock on the range [200, 300] in dir/file_100M"
writelock "dir/file_100M" 200 100
writelocks[1]=$!

# Verify 2 locks are active: one shared lock and an exclusive one
assert_active_eq 2
saunafs_admin_master manage-locks list posix --porcelain --active

echo "Release the shared lock on the range [0, 100] in dir/file_100M"
unlock ${readlocks[1]}

# Verify only the exclusive lock is active
assert_active_eq 1
saunafs_admin_master manage-locks list posix --porcelain --active

echo "Acquire an exclusive lock on the range [50, 150] in dir/file_100M"
writelock "dir/file_100M" 50 100
writelocks[2]=$!

# Verify 2 exclusive locks are active
assert_active_eq 2
saunafs_admin_master manage-locks list posix --porcelain --active

echo "Release the exclusive lock on the range [200, 300] in dir/file_100M"
unlock ${writelocks[1]}

echo "Release the exclusive lock on the range [50, 200] in dir/file_100M"
unlock ${writelocks[2]}

echo "Acquire an exclusive lock for the file dir/file_100M"
writelock "dir/file_100M" 0 0
writelocks[3]=$!

# Verify only the exclusive lock for the file dir/file_100M is active
assert_active_eq 1
saunafs_admin_master manage-locks list posix --porcelain --active

echo "Release the exclusive lock for the file dir/file_100M"
unlock ${writelocks[3]}

# Final verification
assert_active_eq 0
saunafs_admin_master manage-locks list posix --porcelain --active

test_error_cleanup

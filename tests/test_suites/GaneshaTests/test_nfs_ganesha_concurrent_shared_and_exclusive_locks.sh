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
FILE_SIZE="10M" assert_success file-generate "${TEMP_DIR}/mnt/ganesha/dir/file_test"

operations=0

function assert_operation_performed() {
	operations=$((operations + 1))
	assert_eventually_prints "$1" "sed -n ${operations}p ${TEMP_DIR}/posixlock.log"
}

function readlock() {
	posixlockcmd $1 r $2 $3 >> "${TEMP_DIR}/posixlock.log" &
	assert_operation_performed "read  open:   $1"
}

function writelock() {
	posixlockcmd $1 w $2 $3 >> "${TEMP_DIR}/posixlock.log" &
	assert_operation_performed "write open:   $1"
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

declare -a sharedLocks
declare -a exclusiveLocks

# Go to the Ganesha mount point
cd "${TEMP_DIR}/mnt/ganesha"

echo "Acquire a shared lock on the range [0, 100] in dir/file_test"
readlock "dir/file_test" 0 100
sharedLocks[1]=$!
assert_operation_performed "read  lock:   dir/file_test"

echo "Acquire an exclusive lock on the range [200, 300] in dir/file_test"
writelock "dir/file_test" 200 100
exclusiveLocks[1]=$!
assert_operation_performed "write lock:   dir/file_test"

# Verify 2 locks are active: one shared lock and an exclusive one
assert_active_eq 2
saunafs_admin_master manage-locks list posix --porcelain --active

# Acquire a shared lock on [200, 250] (should block/fail due to overlapping with exclusive lock)
echo "Attempt to acquire a shared lock on the range [200, 250] in dir/file_test"
readlock "dir/file_test" 200 50
sharedLocks[2]=$!

# Wait a little bit for the server to queue the shared lock
sleep 2

# Verify 2 locks are active: one shared lock and an exclusive one
assert_active_eq 2
saunafs_admin_master manage-locks list posix --porcelain --active

# In Ganesha v6.5, sometimes reopen_func is not called on time by all the threads.
# This prevents the renewal of the fd, making the fcntl() function to fail at the
# moment of releasing the exclusive lock. This behavior makes the test flaky.
# Reading the file triggers a reopen_func and a renewal of the fd, reducing the likelihood
# of the previous flaky behavior.
md5sum dir/file_test > /dev/null

echo "Release exclusive lock on the range [200, 300] in dir/file_test"
unlock "${exclusiveLocks[1]}"
assert_operation_performed "write unlock: dir/file_test"

# After releasing exclusive lock, second shared lock should be acquired
assert_operation_performed "read  lock:   dir/file_test"

# Wait a little bit for the server to process the lock/unlock operations
sleep 2

# Verify there are 2 active shared locks
assert_active_eq 2
saunafs_admin_master manage-locks list posix --porcelain --active

echo "Release shared lock on the range [0, 100] in dir/file_test"
unlock "${sharedLocks[1]}"
assert_operation_performed "read  unlock: dir/file_test"

# Wait a little bit for the server to process the lock/unlock operations
sleep 2

# Verify only 1 lock is active (the shared one)
assert_active_eq 1
saunafs_admin_master manage-locks list posix --porcelain --active

echo "Release shared lock on the range [200, 250] in dir/file_test"
unlock "${sharedLocks[2]}"
assert_operation_performed "read  unlock: dir/file_test"

# Final verification
assert_active_eq 0
saunafs_admin_master manage-locks list posix --porcelain --active
echo "Concurrent locking test passed successfully!!!"

test_error_cleanup

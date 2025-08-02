timeout_set 2 minutes

USE_RAMDISK=YES \
	setup_local_empty_saunafs info

test_error_cleanup() {
	cd ${TEMP_DIR}
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

function assert_operation_not_performed() {
	assert_eventually_prints "" "sed -n ${1}p ${TEMP_DIR}/posixlock.log"
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

declare -a sharedLocks
declare -a exclusiveLocks

declare -a lockIndexes=(1 2 3)

# Go to the Ganesha mount point
cd "${TEMP_DIR}/mnt/ganesha"

# Acquire 3 shared locks on [100, 200]
for i in "${lockIndexes[@]}"; do
	readlock "dir/file_test" 100 100
    sharedLocks[$i]=$!
    assert_operation_performed "read  lock:   dir/file_test"
done

# Attempt exclusive lock on [100, 200] (should block/fail)
writelock "dir/file_test" 100 100
exclusiveLocks[1]=$!
assert_operation_not_performed $((operations+1)) # Should not acquire

# Release shared locks
for i in "${lockIndexes[@]}"; do
    unlock "${sharedLocks[$i]}"
	assert_operation_performed "read  unlock: dir/file_test"
done

# Now exclusive lock should be acquired
assert_operation_performed "write lock:   dir/file_test"

# Attempt shared lock while exclusive lock is held (should block/fail)
readlock "dir/file_test" 100 100
sharedLocks[4]=$!
assert_operation_not_performed $((operations+1)) # Should not acquire

# Release exclusive lock
unlock "${exclusiveLocks[1]}"
assert_operation_performed "write unlock: dir/file_test"

# Now shared lock should be acquired
assert_operation_performed "read  lock:   dir/file_test"

# Release shared lock
unlock "${sharedLocks[4]}"
assert_operation_performed "read  unlock: dir/file_test"

test_error_cleanup

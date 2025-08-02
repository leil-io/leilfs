timeout_set 1 minute

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
FILE_SIZE="100M" assert_success file-generate "${TEMP_DIR}/mnt/ganesha/dir/file_100M"

opcount=0

function assert_operation_performed() {
	opcount=$((opcount + 1))
	assert_eventually_prints "$1" "sed -n ${opcount}p ${TEMP_DIR}/posixlock.log"
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

declare -a readlocks
declare -a writelocks

# Go to the Ganesha mount point
cd "${TEMP_DIR}/mnt/ganesha"

readlock "dir/file_100M" 0 100
readlocks[1]=$!
assert_operation_performed "read  lock:   dir/file_100M"

readlock "dir/file_100M" 0 100
readlocks[2]=$!

assert_operation_performed "read  lock:   dir/file_100M"

# Lock byte range [200, 300]
writelock "dir/file_100M" 200 100
writelocks[1]=$!
assert_operation_performed "write lock:   dir/file_100M"

unlock ${readlocks[1]}
assert_operation_performed "read  unlock: dir/file_100M"

# Lock byte range [50, 150]
writelock "dir/file_100M" 50 100
writelocks[2]=$!

unlock ${readlocks[2]}
assert_operation_performed "read  unlock: dir/file_100M"

assert_operation_performed "write lock:   dir/file_100M"

# Lock byte range [0, 0] is equivalent to locking the whole file
writelock "dir/file_100M" 0 0
writelocks[3]=$!

# It's not possible to acquire the lock because range [50, 150] is locked
assert_operation_not_performed $((opcount+1))

# Unlock byte range [200, 300]
unlock ${writelocks[1]}
assert_operation_performed "write unlock: dir/file_100M"

# Unlock byte range [50, 150]
unlock ${writelocks[2]}
assert_operation_performed "write unlock: dir/file_100M"

# Now, it's possible to lock the whole file
assert_operation_performed "write lock:   dir/file_100M"

# Unlock byte range [0, 0]
unlock ${writelocks[3]}
assert_operation_performed "write unlock: dir/file_100M"

test_error_cleanup

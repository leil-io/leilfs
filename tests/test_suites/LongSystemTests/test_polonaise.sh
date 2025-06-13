timeout_set '30 minutes'

assert_program_installed git
assert_program_installed cmake
assert_program_installed saunafs-polonaise-server
assert_program_installed polonaise-fuse-client

CHUNKSERVERS=1 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER,readbuffersexpirationtime=100,maxreadaheadrequests=1,readworkers=10" \
	setup_local_empty_saunafs info

MINIMUM_PARALLEL_JOBS=4
MAXIMUM_PARALLEL_JOBS=16
PARALLEL_JOBS=$(get_nproc_clamped_between ${MINIMUM_PARALLEL_JOBS} ${MAXIMUM_PARALLEL_JOBS})

# Start Polonaise
saunafs-polonaise-server \
	--master-host=localhost \
	--master-port=${info[matocl]} \
	--bind-port 9090 &> /dev/null &
sleep 3
mnt="$TEMP_DIR/sfspolon"
mkdir -p "$mnt"
polonaise-fuse-client "$mnt" -o allow_other &
assert_eventually 'saunafs dirinfo "$mnt"'

# Perform a compilation
cd "$mnt"
assert_success git clone https://github.com/saunafs/saunafs.git
mkdir saunafs/build
cd saunafs/build
assert_success cmake .. -DCMAKE_INSTALL_PREFIX="$mnt"
make -j${PARALLEL_JOBS} install

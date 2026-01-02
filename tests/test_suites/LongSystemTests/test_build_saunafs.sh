timeout_set 10 minutes
assert_program_installed git
assert_program_installed cmake

CHUNKSERVERS=8 \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER|cacheexpirationtime=0" \
	CHUNKSERVER_EXTRA_CONFIG="READ_AHEAD_KB = 1024|MAX_READ_BEHIND_KB = 2048"
	setup_local_empty_saunafs info

MINIMUM_PARALLEL_JOBS=4
MAXIMUM_PARALLEL_JOBS=16
PARALLEL_JOBS=$(get_nproc_clamped_between ${MINIMUM_PARALLEL_JOBS} ${MAXIMUM_PARALLEL_JOBS})

echo "DEBUG 1"

cd ${info[mount0]}

mkdir work
saunafs setgoal ec43 work

echo "DEBUG 2"

cd work

assert_success git clone "https://github.com/leil-io/saunafs.git"

echo "DEBUG 3"

echo "SOURCE_DIR: ${SOURCE_DIR}"
echo "SOURCE_DIR: ${TEMP_DIR}"
cp -r "${SOURCE_DIR}" "${TEMP_DIR}" || true
SAUNAFS_FOLDER=$(basename "${SOURCE_DIR}")
cd "${TEMP_DIR}/${SAUNAFS_FOLDER}" || exit 1

rm -rf build
mkdir -p build
echo "DEBUG 4"
cd build
echo "DEBUG 5"

echo "VCPKG_ROOT: ${VCPKG_ROOT}"

assert_success cmake .. \
    -G 'Unix Makefiles' \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_TOOLCHAIN_FILE="../vcpkg/scripts/buildsystems/vcpkg.cmake"

assert_success make -j${PARALLEL_JOBS}

timeout_set 1 hour
assert_program_installed git
assert_program_installed cmake

CHUNKSERVERS=3 \
	MOUNTS=1 \
	CHUNKSERVER_EXTRA_CONFIG="MASTER_RECONNECTION_DELAY = 1" \
	MASTER_EXTRA_CONFIG="AUTO_RECOVERY = 1"\
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER|cacheexpirationtime=0,sfsdebug" \
	setup_local_empty_saunafs info

MINIMUM_PARALLEL_JOBS=5
MAXIMUM_PARALLEL_JOBS=16
PARALLEL_JOBS=$(get_nproc_clamped_between ${MINIMUM_PARALLEL_JOBS} ${MAXIMUM_PARALLEL_JOBS})

master_kill_loop() {
	while true; do
		saunafs_stop_master_without_saving_metadata
		saunafs_master_daemon start
		saunafs_wait_for_all_ready_chunkservers
		sleep 5
	done
}

# Daemonize the master kill loop
( master_kill_loop & )

cd "${info[mount0]}"
# assert_success git clone https://github.com/leil-io/saunafs.git
	cp -r "${SOURCE_DIR}" ./ || true
	SAUNAFS_FOLDER=$(basename "${SOURCE_DIR}")
	saunafs setgoal -r 2 "${SAUNAFS_FOLDER}"
	
	cd "${SAUNAFS_FOLDER}" || exit 1
	
	rm -rf build
	mkdir -p build

	cd build

assert_success cmake .. \
    -G 'Unix Makefiles' \
    -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX=../install \
    -DCMAKE_TOOLCHAIN_FILE="../vcpkg/scripts/buildsystems/vcpkg.cmake"

assert_success make -j${PARALLEL_JOBS} install

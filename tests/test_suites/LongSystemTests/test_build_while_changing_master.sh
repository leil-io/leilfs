timeout_set '40 minutes'

metaservers_nr=2
MASTERSERVERS=$metaservers_nr \
	CHUNKSERVERS=2 \
	CHUNKSERVER_EXTRA_CONFIG="MASTER_RECONNECTION_DELAY = 1" \
	SFSEXPORTS_EXTRA_OPTIONS="allcanchangequota" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER|cacheexpirationtime=0,sfsdebug" \
	MASTER_EXTRA_CONFIG="MAGIC_AUTO_FILE_REPAIR = 1" \
	setup_local_empty_saunafs info

MINIMUM_PARALLEL_JOBS=5
MAXIMUM_PARALLEL_JOBS=16
PARALLEL_JOBS=$(get_nproc_clamped_between ${MINIMUM_PARALLEL_JOBS} ${MAXIMUM_PARALLEL_JOBS})

assert_program_installed git
assert_program_installed cmake

master_kill_loop() {
	# Start shadow masters
	for ((shadow_id=1 ; shadow_id<metaservers_nr; ++shadow_id)); do
		saunafs_master_n $shadow_id start
		echo DEBUG "master_kill_loop_for"
		assert_eventually "saunafs_shadow_synchronized $shadow_id" '2 minutes'
	done

	loop_nr=0
	# Let the master run for few seconds and then replace it with another one
	while true; do
		echo "Loop nr $loop_nr"
		sleep 5

		prev_master_id=$((loop_nr % metaservers_nr))
		new_master_id=$(((loop_nr + 1) % metaservers_nr))
		loop_nr=$((loop_nr + 1))

		# Kill the previous master
		assert_eventually "saunafs_shadow_synchronized $new_master_id" '2 minutes'
		saunafs_stop_master_without_saving_metadata
		saunafs_make_conf_for_shadow $prev_master_id

		# Promote a next master
		saunafs_make_conf_for_master $new_master_id
		saunafs_master_daemon reload

		# Demote previous master to shadow
		saunafs_make_conf_for_shadow $prev_master_id
		saunafs_master_n $prev_master_id start
		assert_eventually "saunafs_shadow_synchronized $prev_master_id" '2 minutes'

		saunafs_wait_for_all_ready_chunkservers
	done
}

# Daemonize the master kill loop
master_kill_loop &

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
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
	-DCMAKE_INSTALL_PREFIX=../install \
    -DCMAKE_TOOLCHAIN_FILE="../vcpkg/scripts/buildsystems/vcpkg.cmake"


assert_success make -j${PARALLEL_JOBS} install

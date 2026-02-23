timeout_set 45 seconds
valgrind_enable "helgrind"

CHUNKSERVERS=3 \
	DISK_PER_CHUNKSERVER=2 \
	USE_RAMDISK=YES \
	MASTER_CUSTOM_GOALS="10 ec21: \$ec(2,1)" \
	# increased timeouts due to expected slowdown by helgrind
	MOUNT_EXTRA_CONFIG="sfschunkserverwavereadto=5000,`
		`sfschunkservertotalreadto=20000,maxreadaheadrequests=2,`
		`cacheexpirationtime=60000" \
	AUTO_SHADOW_MASTER="NO" \
	setup_local_empty_saunafs info

number_of_files=4
file_size=$((1024 * 1024 * 8))
goals="ec21"

function generateFiles() {
	for goal in ${goals}; do
		mkdir ${goal}
		saunafs setgoal ${goal} ${goal}

		echo "Writing ${number_of_files} files with goal ${goal}"
		for i in $(seq 1 ${number_of_files}); do
			FILE_SIZE=${file_size} assert_success file-generate "${goal}/file${i}" &
		done
	done

	wait

	echo "Files written"
}

function validateFiles() {
	for goal in ${goals}; do
		echo "Validating ${number_of_files} files with goal ${goal}"
		for i in $(seq 1 ${number_of_files}); do
			assert_success file-validate "${goal}/file${i}" &
		done
	done

	wait

	echo "Files validated"
}

cd "${info[mount0]}"

generateFiles

# FIX(Dave, Guillex): This validateFiles is causing timeouts on some machines. Sometimes it
# finishes validating files and sometimes it gets stuck
# drop_caches
# validateFiles

saunafs_chunkserver_daemon 0 stop

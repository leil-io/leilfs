timeout_set 15 minutes

sqlite_test_path=$(realpath test_utils/sqlite_stress_test.py)

CHUNKSERVERS=5 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER,readbuffersexpirationtime=100,maxreadaheadrequests=1,readworkers=10,sfsdirentrycacheto=0" \
	CHUNKSERVER_EXTRA_CONFIG="MAGIC_DEBUG_LOG=${TEMP_DIR}/log" \
	setup_local_empty_saunafs info

cd ${info[mount0]}

run_sqlite_test_with_parameters() {
	local nr_of_threads=$1
	local nr_of_operations=$2
	assert_success python3 "${sqlite_test}" --threads=${nr_of_threads} \
		--operations_per_thread=${nr_of_operations} --seed=1 --db_file="$(realpath db)"
	echo "SQLite stress test with ${nr_of_threads} threads and ${nr_of_operations} operations per" \
		"thread completed successfully."
}

# Test with different goals
goals=(std 3 ec32)

for goal in "${goals[@]}"; do
	echo "Running tests with goal ${goal}"

	dir_name=dir_${goal}
	mkdir ${dir_name}
	if [[ "$goal" != "std" ]]; then
		saunafs setgoal -r ${goal} ${dir_name}
	fi
	cd "${dir_name}"

	cp "${sqlite_test_path}" .
	sqlite_test=./sqlite_stress_test.py
	chmod +x "${sqlite_test}"

	touch db

	run_sqlite_test_with_parameters 10 10
	run_sqlite_test_with_parameters 10 100
	run_sqlite_test_with_parameters 100 10
	run_sqlite_test_with_parameters 100 100
	run_sqlite_test_with_parameters 100 200
	run_sqlite_test_with_parameters 200 100
	run_sqlite_test_with_parameters 200 200

	# Go back to the mount point
	cd ..
done

# Search for "[error]" in the chunkserver logs
if grep -F '[error]' "${TEMP_DIR}/log"; then
	test_fail "Error: Found errors in the chunkserver logs!"
fi

timeout_set 1 hour

sqlite_test_path=$(realpath test_utils/sqlite_stress_test.py)

CHUNKSERVERS=1 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

cd ${info[mount0]}

cp $sqlite_test_path .
sqlite_test=./sqlite_stress_test.py
chmod +x $sqlite_test

touch db

run_sqlite_test_with_parameters() {
	local nr_of_threads=$1
	local nr_of_operations=$2
	python3 $sqlite_test --threads=$nr_of_threads --operations_per_thread=$nr_of_operations \
	    --seed=1 --db_file=$(realpath db) |& tee $TEMP_DIR/sqlite_test.log
	assert_equals 1 $(grep "Test completed without fatal errors." $TEMP_DIR/sqlite_test.log | wc -l)
}

run_sqlite_test_with_parameters 200 2000

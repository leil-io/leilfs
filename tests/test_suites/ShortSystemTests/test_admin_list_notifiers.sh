USE_RAMDISK=YES \
	setup_local_empty_saunafs info

for i in {1..4}; do
	# Start the metadata notifier which will log all the messages to a file
	metadata-notifier localhost "${info[matont]}" > "${TEMP_DIR}/notifier_${i}.log" 2>&1 &
done

pgrep -fa metadata-notifier

notifiers=$(saunafs_admin_command list-inotifiers localhost "${info[matocl]}")
echo "Notifiers listed by admin command:"
echo "$notifiers"
expect_equals "4" "$(echo "$notifiers" | grep -c '^Server ')"

# Perform some metadata operation to generate notifications
mkdir "${info[mount0]}/test_folder"

# Print each log file
for i in {1..4}; do
	echo "===== notifier_${i}.log ====="
	cat "${TEMP_DIR}/notifier_${i}.log"
done

# Check if all log files are equal
first_log="${TEMP_DIR}/notifier_1.log"
for i in {2..4}; do
	assert_equals "$(cat $first_log)" "$(cat ${TEMP_DIR}/notifier_${i}.log)"; 
done

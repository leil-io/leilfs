CHUNKSERVERS=1 \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

mkdir folder1
mkdir folder2
mkdir folder1/subfolder1
mkdir folder1/subfolder2

# Start the metadata notifier which will log all the messages to a file
metadata-notifier localhost "${info[matont]}" > "${info[mount0]}/notifier.log" 2>&1 &
NOTIFIER_PID=$!
echo "Notifier started with PID $NOTIFIER_PID"
echo "Pgrep notifier PID $(pgrep -fa metadata-notifier)"

# Now perform some operations to generate metadata change notifications
folders=("." "folder1" "folder2" "folder1/subfolder1" "folder1/subfolder2")
for f in "${folders[@]}"; do
    ls "$f"
    sleep 1
done

# Stop the notifier and print the log file
kill -s SIGKILL $NOTIFIER_PID
cat "${info[mount0]}/notifier.log"

# Assert expected substrings
expected=("ACCESS(1)" "ACCESS(2)" "ACCESS(3)" "ACCESS(4)" "ACCESS(5)")
for e in "${expected[@]}"; do
    assert_success grep -q "$e" "${info[mount0]}/notifier.log"
done

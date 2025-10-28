CHUNKSERVERS=1 \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

# Start the metadata notifier which will log all the messages to a file
metadata-notifier localhost "${info[matont]}" > "${TEMP_DIR}/notifier.log" 2>&1 &
NOTIFIER_PID=$!
echo "Notifier started with PID $NOTIFIER_PID"

# Verify that the notifier process is running
assert_eventually "ps -p $NOTIFIER_PID -o cmd= | grep -q metadata-notifier"

cd "${info[mount0]}"

mkdir folder1
mkdir folder2
mkdir folder1/subfolder1
mkdir folder1/subfolder2

# Now perform some operations to generate metadata change notifications
folders=("." "folder1" "folder2" "folder1/subfolder1" "folder1/subfolder2")
for f in "${folders[@]}"; do
    ls "$f"
    sleep 1
done

# Stop the notifier and print the log file
kill -s SIGKILL $NOTIFIER_PID
cat "${TEMP_DIR}/notifier.log"

# Assert expected substrings
expected=(
    "ACCESS(1)"
    "inode 1: type=d path=/"
    "ACCESS(2)"
    "inode 2: type=d path=/folder1"
    "ACCESS(3)"
    "inode 3: type=d path=/folder2"
    "ACCESS(4)"
    "inode 4: type=d path=/folder1/subfolder1"
    "ACCESS(5)"
    "inode 5: type=d path=/folder1/subfolder2"
)
for e in "${expected[@]}"; do
    assert_success grep -q "$e" "${TEMP_DIR}/notifier.log"
done

compare_dates() {
    local expected_date="$1"
    local actual_date="$2"
    local tolerance="$3"

    # Replace underscores with spaces in the date strings
    expected_date=$(echo "$expected_date" | sed 's/_/ /g')
    actual_date=$(echo "$actual_date" | sed 's/_/ /g')

    # Convert the expected and actual dates to Unix timestamps
    expected_timestamp=$(date -d "$expected_date" +%s)
    actual_timestamp=$(date -d "$actual_date" +%s)

    # Compare the timestamps using the assert_near function with the specified tolerance
    assert_near "$expected_timestamp" "$actual_timestamp" "$tolerance"
}

USE_RAMDISK=YES \
    setup_local_empty_saunafs info

if is_windows_system; then
    # Get the PID and username of the sfsmount process on Windows
    tasklist_output=$(tasklist.exe /FI "IMAGENAME eq sfsmount.exe" 2>&1)
    sfsmount_pid=$(echo "$tasklist_output" | awk '/sfsmount.exe/ {print $2}')
    expected_username="USERNAME: $(whoami.exe | tr -d '\r' | cut -d'\' -f2)"
    expected_sid="SID: $(whoami.exe /user | grep -A 1 '=========== ' | tail -n 1 | awk '{print $2}' | tr -d '\r')"
else
    # Get the PID of the sfsmount process on Linux
    sfsmount_pid=$(pgrep -fa sfsmount | grep -v grep | awk '{print $1}')
    expected_username="USERNAME: $(id -un)"
    expected_uid="UID: $(id -u)"
    expected_gid="GID: $(id -g)"
fi

expected_started_date="$(date '+%Y-%m-%d_%H:%M:%S')"
expected_pid="PID: $sfsmount_pid"

# Read the .saunafs_mount_info file
mount_info=$(cat "${info[mount0]}/.saunafs_mount_info")

# Check if the contents match the expected values
actual_started_date=$(echo "$mount_info" | egrep "STARTED DATE" | awk '{print $3}')
compare_dates "$expected_started_date" "$actual_started_date" 5
if is_windows_system; then
    assert_success $(echo "$mount_info" | grep -q "$expected_sid")
else
    assert_success $(echo "$mount_info" | grep -q "$expected_uid")
    assert_success $(echo "$mount_info" | grep -q "$expected_gid")
fi
assert_success $(echo "$mount_info" | grep -iq "$expected_username")
assert_success $(echo "$mount_info" | grep -q "$expected_pid")

# Check if the specified values produce some output and are not empty
assert_success $(echo "$mount_info" | grep -q "VERSION: ")
assert_success $(echo "$mount_info" | grep -q "COMMIT_ID: ")
assert_success $(echo "$mount_info" | grep -q "ARGUMENTS: ")
assert_success $(echo "$mount_info" | grep -q "MOUNT OPTIONS:")

# Check if tweaks are correctly updated
assert_equals $(cat "${info[mount0]}/.saunafs_tweaks" | egrep CacheExpirationTime | awk '{print $2}') $(cat "${info[mount0]}/.saunafs_mount_info" | egrep cacheexpirationtime | awk '{print $2}')
assert_equals $(cat "${info[mount0]}/.saunafs_tweaks" | egrep WriteMaxRetries | awk '{print $2}') $(cat "${info[mount0]}/.saunafs_mount_info" | egrep sfsioretries | awk '{print $2}')
assert_equals $(cat "${info[mount0]}/.saunafs_tweaks" | egrep MaxReadaheadRequests | awk '{print $2}') $(cat "${info[mount0]}/.saunafs_mount_info" | egrep maxreadaheadrequests | awk '{print $2}')

echo "CacheExpirationTime=251" | sudo tee "${info[mount0]}/.saunafs_tweaks"
echo "WriteMaxRetries=55" | sudo tee "${info[mount0]}/.saunafs_tweaks"
echo "MaxReadaheadRequests=23" | sudo tee "${info[mount0]}/.saunafs_tweaks"

assert_equals $(cat "${info[mount0]}/.saunafs_tweaks" | egrep CacheExpirationTime | awk '{print $2}') $(cat "${info[mount0]}/.saunafs_mount_info" | egrep cacheexpirationtime | awk '{print $2}')
assert_equals $(cat "${info[mount0]}/.saunafs_tweaks" | egrep WriteMaxRetries | awk '{print $2}') $(cat "${info[mount0]}/.saunafs_mount_info" | egrep sfsioretries | awk '{print $2}')
assert_equals $(cat "${info[mount0]}/.saunafs_tweaks" | egrep MaxReadaheadRequests | awk '{print $2}') $(cat "${info[mount0]}/.saunafs_mount_info" | egrep maxreadaheadrequests | awk '{print $2}')

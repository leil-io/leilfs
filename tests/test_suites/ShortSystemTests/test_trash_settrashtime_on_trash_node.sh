timeout_set 1 minute

# Regression test for a master crash triggered by calling settrashtime on a file that is
# already resident in the trash.

MOUNTS=2 \
	CHUNKSERVERS=1 \
	USE_RAMDISK=YES \
	SFSEXPORTS_META_EXTRA_OPTIONS="nonrootmeta" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	MOUNT_1_EXTRA_CONFIG="sfsmeta" \
	setup_local_empty_saunafs info

trash="${info[mount1]}/trash"
regular="${info[mount0]}"

# Create a file with a long trashtime so it stays in trash for the duration of the test.
cd "${regular}"
echo "content" > testfile
oneDayInseconds=$((24 * 60 * 60))
saunafs settrashtime ${oneDayInseconds} testfile
rm testfile

# Wait for the deleted file to appear in the trash via the meta mount.
assert_eventually 'test "$(ls "${trash}" | grep -v undel | wc -l)" -eq 1'

file_in_trash="${trash}/$(ls "${trash}" | grep -v undel | head -1)"

# Change the trashtime of the file while it is already inside the trash.
assert_success saunafs settrashtime 7200 "${file_in_trash}"

# ls the trash directory to trigger the master to read the node with the updated trashtime.
assert_success ls "${trash}"

# Confirm the master is still alive.
assert_success saunafs_master_daemon isalive

# undel the file and check it is there
assert_success mv "${file_in_trash}" "${trash}/undel"
assert_success test -f "${regular}/testfile"

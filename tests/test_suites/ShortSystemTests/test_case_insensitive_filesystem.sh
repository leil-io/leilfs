timeout_set "1 minute"

master_cfg="METADATA_DUMP_PERIOD_SECONDS = 0"
master_cfg+="|MAGIC_DEBUG_LOG = ${TEMP_DIR}/log|LOG_FLUSH_ON=DEBUG"
master_cfg+="|METADATA_SAVE_REQUEST_MIN_PERIOD = $(timeout_rescale_seconds 10)"

USE_RAMDISK=YES \
	SFSEXPORTS_EXTRA_OPTIONS="caseinsensitive" \
	MASTER_EXTRA_CONFIG="$master_cfg" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

# create folder and sample files
mkdir folder1
touch file1
touch folder1/file2

# test write case insensitive
echo "data" > fIlE1
assert_equals "data" "$(cat file1)"

echo "data" > fOLDER1/FIle2
assert_equals "data" "$(cat folder1/file2)"

# test read case insensitive
assert_equals "data" "$(cat FilE1)"
assert_equals "data" "$(cat fOlDeR1/fILe2)"

# test rename case insensitive same file
# expected behavior is that if target file exists, it is overwritten
# but its name case is preserved, because otherwise it would send
# inconsistent information to shadow masters
touch file3
mv file3 filE1

# test rename case insensitive new file to existing file
touch file3
sleep 1
assert_equals "3" "$(ls . | wc -l)"
mv file3 file1
assert_equals "2" "$(ls . | wc -l)"

# test remove case insensitive
rm "fiLE1"
assert_equals "1" "$(ls . | wc -l)"

rm "foldeR1/fIlE2"
assert_equals "0" "$(ls foldeR1/ | wc -l)"

# test recursive case insensitive in directories
mkdir folder2
mkdir folder2/folder3
mkdir folder2/folder3/folder4

touch FOldER2/fOLdeR3/folDEr4/file4

echo "data" > FOldER2/fOLdeR3/folDEr4/fIlE4

assert_equals "data" "$(cat folder2/folder3/folder4/file4)"
assert_equals "data" "$(cat folder2/folder3/folder4/fIlE4)"

if [[ ${info[is_windows_system]} -ne 1 ]]; then
	# test hardlink case insensitive
	ln FOldER2/fOLdeR3/folDEr4/fIlE4 file4_hardlink
	assert_equals "data" "$(cat file4_HARDLINK)"

	# test symlink case insensitive
	ln -s FOldER2/fOLdeR3/folDEr4/fIlE4 file4_symlink
	assert_equals "data" "$(cat FILE4_SYMLINK)"

	# test creating a broken symlink (target does not exist)
	ln -s NonExistent/Path/File broken_link
	assert_failure cat broken_link
fi

# test removing case insensitive in mountpoint
cd ..
saunafs_mount_unmount 0
assert_success saunafs_master_daemon stop
sleep 5
SFSEXPORTS_EXTRA_OPTIONS=""
number_of_mounts=1
rm "$TEMP_DIR/saunafs/etc/sfsexports.cfg"
create_sfsexports_cfg_ >"$TEMP_DIR/saunafs/etc/sfsexports.cfg"
saunafs_master_daemon start
saunafs_mount_start 0

cd "${info[mount0]}"
assert_equals "data" "$(cat folder2/folder3/folder4/file4)"
assert_failure cat folder2/folder3/folder4/fIlE4

touch file5
echo "data" > file5
assert_success cat file5
assert_failure cat fiLE5

if [[ ${info[is_windows_system]} -ne 1 ]]; then
	# test hardlink after disabling case insensitive
	assert_failure cat file4_HARDLINK
	assert_equals "data" "$(cat file4_hardlink)"

	# test symlink after disabling case insensitive
	assert_failure cat FILE4_SYMLINK
	assert_equals "data" "$(cat file4_symlink)"
fi

# create new file and check normal behavior
touch TestFile
echo "data" > TestFile
assert_equals "data" "$(cat TestFile)"
assert_failure cat testfile

# enable case insensitive again, for ensuring that 
# previous files are accessible
cd ..
saunafs_mount_unmount 0
assert_success saunafs_master_daemon stop
sleep 5
SFSEXPORTS_EXTRA_OPTIONS="caseinsensitive"
number_of_mounts=1
rm "$TEMP_DIR/saunafs/etc/sfsexports.cfg"
create_sfsexports_cfg_ >"$TEMP_DIR/saunafs/etc/sfsexports.cfg"
saunafs_master_daemon start
saunafs_mount_start 0

cd "${info[mount0]}"
# test previous created file with case insensitive enabled
assert_equals "data" "$(cat TESTFILE)"
assert_equals "data" "$(cat testfile)"

# Verify that no checksum or changelog apply errors were logged
log=$(cat "${TEMP_DIR}/log")
assert_awk_finds_no '/master.mismatch/' "$log"
assert_awk_finds_no '/master.fs.checksum.mismatch/' "$log"
assert_awk_finds_no '/master.mltoma_changelog_apply_error/' "$log"

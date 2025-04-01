timeout_set 2 minutes
CHUNKSERVERS=3 \
	USE_RAMDISK=YES \
	SFSEXPORTS_EXTRA_OPTIONS="allcanchangequota" \
	MASTER_EXTRA_CONFIG="METADATA_DUMP_PERIOD_SECONDS = 0|AUTO_RECOVERY = 1" \
	setup_local_empty_saunafs info

# Remember version of the metadata file. We expect it not to change when generating data.
metadata_version=$(metadata_get_version "${info[master_data_path]}"/metadata.sfs)

cd ${info[mount0]}
mkdir dir
saunafs setgoal 3 dir
FILE_SIZE=$(( 4 * SAUNAFS_CHUNK_SIZE )) file-generate dir/file

wait_if_windows

assert_equals 4 $(find_chunkserver_metadata_chunks 0 | wc -l)
assert_equals 4 $(find_chunkserver_metadata_chunks 1 | wc -l)
assert_equals 4 $(find_chunkserver_metadata_chunks 2 | wc -l)

# Stop chunkserver0, and remove third chunk from it.
saunafs_chunkserver_daemon 0 stop
chunk=$(find_chunkserver_metadata_chunks 0 -name "chunk_0000000000000003_00000001.???")
assert_success rm "$chunk"

# Update first and second chunk of the file, this will change it's version to 2 on CS 1 and 2.
dd if=/dev/zero of=dir/file conv=notrunc bs=32KiB count=$((2*1024 + 10))

wait_if_windows

for chunk in 1 2; do
	assert_equals 1 $(find_chunkserver_metadata_chunks 0 -name "chunk_000000000000000${chunk}_00000001.???" | wc -l)
	assert_equals 1 $(find_chunkserver_metadata_chunks 1 -name "chunk_000000000000000${chunk}_00000002.???" | wc -l)
	assert_equals 1 $(find_chunkserver_metadata_chunks 2 -name "chunk_000000000000000${chunk}_00000002.???" | wc -l)
done

# Stop chunkserver 1 and remove third chunk from it.
saunafs_chunkserver_daemon 1 stop
chunk=$(find_chunkserver_metadata_chunks 1 -name "chunk_0000000000000003_00000001.???")
assert_success rm "$chunk"

# Update first and second chunk of the file, this will change it's version to 3 on CS 2.
dd if=/dev/zero of=dir/file conv=notrunc bs=32KiB count=$((2*1024 + 10))

wait_if_windows

for chunk in 1 2; do
	assert_equals 1 $(find_chunkserver_metadata_chunks 0 -name "chunk_000000000000000${chunk}_00000001.???" | wc -l)
	assert_equals 1 $(find_chunkserver_metadata_chunks 1 -name "chunk_000000000000000${chunk}_00000002.???" | wc -l)
	assert_equals 1 $(find_chunkserver_metadata_chunks 2 -name "chunk_000000000000000${chunk}_00000003.???" | wc -l)
done

# Remove second chunk from chunkserver 1.
chunk=$(find_chunkserver_metadata_chunks 1 -name "chunk_0000000000000002_00000002.???")
assert_success rm "$chunk"

# Stop chunkserver 2, turn on chunkservers 0 and 1 again. File should have three chunks lost.
saunafs_chunkserver_daemon 2 stop
saunafs_wait_for_ready_chunkservers 0
saunafs_chunkserver_daemon 0 start
saunafs_chunkserver_daemon 1 start
saunafs_wait_for_ready_chunkservers 2
assert_awk_finds '/chunks with 0 copies: *3$/' "$(saunafs checkfile dir/file)"

# Repair the file and remember metadata after the repair.
repairinfo=$(saunafs filerepair dir/file)
fileinfo=$(saunafs fileinfo dir/file)
assert_awk_finds '/chunks not changed: *1$/' "$repairinfo"
assert_awk_finds '/chunks erased: *1$/' "$repairinfo"
assert_awk_finds '/chunks repaired: *2$/' "$repairinfo"
# Rirst chunk should have version 2 (from CS 1).
assert_awk_finds '/id:1 ver:2/' "$fileinfo"
# Second chunk should have version 1 (from CS 0).
assert_awk_finds '/id:2 ver:1/' "$fileinfo"
assert_awk_finds_no '/chunks with 0 copies/' "$(saunafs checkfile dir/file)"
metadata=$(metadata_print)

# Simulate crash of the master.
cd
saunafs_master_daemon kill

# Make sure changes are in changelog only (ie. that metadata wasn't dumped).
assert_equals "$metadata_version" "$(metadata_get_version "${info[master_data_path]}"/metadata.sfs)"

# Restore the filesystem from changelog by starting master server and check it.
assert_success saunafs_master_daemon start
saunafs_wait_for_ready_chunkservers 2
assert_no_diff "$metadata" "$(metadata_print "${info[mount0]}")"

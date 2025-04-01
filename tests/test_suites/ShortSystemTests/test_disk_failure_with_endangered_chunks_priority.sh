USE_RAMDISK=YES \
	CHUNKSERVERS=3 \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	MASTER_EXTRA_CONFIG="CHUNKS_LOOP_MIN_TIME = 1`
			`|CHUNKS_LOOP_MAX_CPU = 90`
			`|OPERATIONS_DELAY_INIT = 0`
			`|ENDANGERED_CHUNKS_PRIORITY = 0.5"
	setup_local_empty_saunafs info

# Clear not needed paths, every hdd line comes in the form
# [zonefs:]metadataPath | dataPath
paths=()

for cfg_line in $(cat "${info[chunkserver0_hdd]}" "${info[chunkserver1_hdd]}"); do
	if [[ $cfg_line = "|" ]]; then
		continue
	fi

	if [[ $cfg_line = zonefs* ]]; then
		path=$(echo $cfg_line | sed s/zonefs://g)
		paths+="${path} "
	else
		# Only add the path for non emulated SMR drives
		if [[ $cfg_line != *sauna_nullb* ]]; then
			paths+="${cfg_line} "
		fi
	fi
done

# Spoil disks on two chunkservers
for hdd in $paths; do
	chmod -w "$hdd"/*
done

# Create some files in goal 3, try to modify them and expect everything to work
cd "${info[mount0]}"
mkdir dir
saunafs setgoal 3 dir
FILE_SIZE=5M file-generate dir/file{1..10}
assert_success file-validate dir/file{1..10}
assert_success file-overwrite dir/file{1..10}
assert_success file-validate dir/file{1..10}
FILE_SIZE=5M file-generate dir/file{5..15}
assert_success file-validate dir/file{5..15}

# Verify if the files indeed have proper chunks only on a single chunkserver
for file in dir/file*; do
	fileinfo=$(saunafs fileinfo "$file")
	assert_awk_finds    "/copy 1/" "$fileinfo"
	assert_awk_finds_no "/copy 2/" "$fileinfo"
done

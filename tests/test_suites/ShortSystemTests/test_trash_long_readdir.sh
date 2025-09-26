timeout_set "5 minutes"

# Mount1 will be used for meta operations
MOUNTS=2 \
	CHUNKSERVERS=1 \
	USE_RAMDISK=YES \
	SFSEXPORTS_META_EXTRA_OPTIONS="nonrootmeta" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	MOUNT_1_EXTRA_CONFIG="sfsmeta" \
	setup_local_empty_saunafs info

trash="${info[mount1]}/trash"

# Create a folder and set long trashtime on it
# which will be (1 hour before freeing trashed files)
mkdir "${info[mount0]}/dir"
saunafs setgoal 1 "${info[mount0]}/dir"
saunafs settrashtime 3600 "${info[mount0]}/dir"

big-session-metadata-benchmark "${info[mount0]}/dir" "200000" "--create-delete-only"

assert_eventually_prints "200001" "ls $trash | wc -l"

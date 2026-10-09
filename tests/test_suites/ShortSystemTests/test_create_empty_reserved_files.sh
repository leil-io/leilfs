CHUNKSERVERS=1 \
	MOUNTS=2 \
	USE_RAMDISK=YES \
	MASTER_EXTRA_CONFIG="EMPTY_RESERVED_FILES_PERIOD_MSECONDS = 1000"\
	MOUNT_0_EXTRA_CONFIG="sfscachemode=NEVER" \
	MOUNT_1_EXTRA_CONFIG="sfsmeta" \
	SFSEXPORTS_META_EXTRA_OPTIONS="nonrootmeta" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

# Function to count the number of files in a given directory
count_files() {
    local dir_path="$1"
    echo $(ls "$dir_path" | wc -l)
}

# set trash and reserved files folder
trash="${info[mount1]}/trash"
reserved="${info[mount1]}/reserved"

# A metadata dump rotates changelog.sfs (a shadow that rejects a change can request one), so read
# every changelog file and keep each "<version>: <timestamp>|<change>" line once. A failed read
# fails the caller instead of looking like an empty changelog.
master_changelog() {
	local changes
	changes=$(cat "${info[master_data_path]}"/changelog.sfs*) || return 1
	awk -F': ' '!seen[$1]++' <<< "${changes}"
}

mkdir folder

# set folder trash time to 0 for redirecting 
# files in use to reserved files folder
saunafs settrashtime 0 folder

# Keep 5 files open: the mount keeps reporting them in use, so only forced cleanup releases them.
reserved_inodes=()
reserved_fds=()
for i in {1..5}; do
	touch "folder/file${i}"
	exec {reserved_fd}<>"folder/file${i}"
	echo "Data" >&"${reserved_fd}"
	reserved_inodes+=("$(inode_of "folder/file${i}")")
	reserved_fds+=("${reserved_fd}")
done
changelog=$(master_changelog)
changelog_start=$(awk -F': ' '$1 > last {last = $1} END {print last + 0}' <<< "${changelog}")

# delete those 5 files and as trashtime = 0
# then these files are marked as reserved in the system
for i in {1..5}; do
    rm folder/file$i
done

# check files are not in trash folder, 
# just undel folder
trash_files_count=$(count_files $trash)
echo "number of trash files: $trash_files_count"
assert_equals "1" "$trash_files_count"

# check reserved files were correctly deleted
assert_eventually '[[ "$(count_files "${reserved}")" == 0 ]]'

# Counts matching changes newer than the saved version.
count_changes_since_start() {
	local changelog
	changelog=$(master_changelog) || return 1
	awk -F': ' -v start="${changelog_start}" -v change="${1}" \
		'$1 > start && index($2, change) {count++} END {print count + 0}' <<< "${changelog}"
}

# Forced cleanup must release a reserved file, never purge it: changelog replay accepts PURGE
# only for a trash file, so a shadow would fail to apply it.
for inode in "${reserved_inodes[@]}"; do
	assert_equals 0 "$(count_changes_since_start "|PURGE(${inode})")"
	assert_eventually "[[ \$(count_changes_since_start '|RELEASE(${inode},') == 1 ]]"
done

for reserved_fd in "${reserved_fds[@]}"; do
	exec {reserved_fd}<&-
done

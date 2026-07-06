metadata_print() {
( # This function calls cd, so run it in a subshell
	cd "${1:-.}"
if [[ ! ${DISABLE_PRINTING_XATTRS:-} ]]; then
	assert_program_installed getfattr
fi
	local file
	local format=$'inode %i; type %F; name %n\n'
	format+=$'mask %A; uid %u; gid %g\n'
	format+=$'mtime %Y; ctime %Z\n'
	format+=$'blocks %b; size %s; links %h; device %t,%T\n'
	find . -type f | sort | while read file; do
		saunafs fileinfo "$file" | grep -v $'^\t\t' # remove "copy N" and "no valid copies"
	done
	find . -type f -o -type d | sort | while read file; do
		saunafs getgoal "$file"
		saunafs gettrashtime "$file"
		saunafs geteattr "$file"
		saunafs dirinfo "$file"
if [[ ! ${DISABLE_PRINTING_XATTRS:-} ]]; then
		getfattr -d "$file"
		getfacl "$file"
fi
	done
	find . -type l | sort | while read file; do
		echo "Link: $file -> $(readlink "$file")"
	done
	find . | sort | while read file; do
		stat -c "$format" "$file"
	done
	if [[ $(stat -c "%i" .) == 1 ]]; then
		saunafs repquota -a .
	fi
)
}

# Extract version from metadata file
# Expected argument is location of metadata file
metadata_get_version() {
	sfsmetadump "$1" | awk 'NR==2 {print $6}'
}

metadata_generate_files() {
	for megabytes in 63 290; do
		FILE_SIZE=${megabytes}M file-generate file_gen${megabytes}
	done
	FILE_SIZE=123456789 BLOCK_SIZE=12345 file-generate file_gen
}

metadata_validate_files() {
	assert_success file-validate file_gen*
}

metadata_generate_funny_inodes() {
	mkdir inodes_dir
	touch inodes_dir/file1 inodes_dir/file2
	mkfifo inodes_fifo
	touch inodes_file
	ln inodes_file inodes_link
	ln -s inodes_file inodes_symlink
}

metadata_generate_quotas() {
	saunafs setquota -u $(id -u) 10GB 30GB 0 0 .
	saunafs setquota -g $(id -g) 0 0 10k 20k .
}

metadata_generate_unlink() {
	touch unlink_file{00..99}
	rm -f unlink_file1?
	mkdir unlink_dir
	touch unlink_dir/{00..11}
	rm -r -f unlink_dir
}

metadata_generate_trash_ops() {
	touch trashed_file
	if [[ ${SFS_META_MOUNT_PATH-} ]]; then
		# Hack: create files using saunafs makesnapshot so that they will never be opened
		for i in 1 2 3 4 5; do
				saunafs makesnapshot trashed_file trashed_file_$i
		done
		trashed_file_4_inode=$(inode_of trashed_file_4)
		trashed_file_5_inode=$(inode_of trashed_file_5)
		saunafs settrashtime 60 trashed_file*
		saunafs settrashtime 1 trashed_file_4
		saunafs settrashtime 0 trashed_file_5
		# Open descriptor of trashed_file_5
		exec 150<>trashed_file_5
		rm trashed_file_*
		# Close descriptor of trashed_file_5
		exec 150>&-
		# Generate SETPATH, UNDEL, and PURGE changes
		echo "untrashed_dir/untrashed_file" > "$SFS_META_MOUNT_PATH"/trash/*trashed_file_3
		assert_eventually 'test -e "$SFS_META_MOUNT_PATH"/trash/*untrashed_file' # wait for rename
		mv "$SFS_META_MOUNT_PATH"/trash/*untrashed_file "$SFS_META_MOUNT_PATH"/trash/undel
		mv "$SFS_META_MOUNT_PATH"/trash/*trashed_file_1 "$SFS_META_MOUNT_PATH"/trash/undel
		rm "$SFS_META_MOUNT_PATH"/trash/*trashed_file_2
		# Wait for generation of PURGE for trashed_file_4
		assert_eventually 'grep PURGE\(${trashed_file_4_inode} "${CHANGELOG}"'
		# Wait for generation of RELEASE for reserved trashed_file_5
		assert_eventually 'grep RELEASE\(${trashed_file_5_inode} "${CHANGELOG}"'
		local changelog=$(cat ${CHANGELOG})
		assert_awk_finds '/SETPATH/' "$changelog"
		assert_awk_finds '/UNDEL/' "$changelog"
	fi
}

metadata_generate_setgoal() {
	for goal in 1 2 3 4 ; do
		touch setgoal$goal
		saunafs setgoal $goal setgoal$goal
	done
	mkdir -p setgoal_recursive/dir{1,2}
	chmod 777 setgoal_recursive/dir{1,2}
	touch setgoal_recursive/dir{1,2}/file1
	sudo -HEnu saunafstest_2 touch setgoal_recursive/dir{1,2}/file2
	saunafs setgoal -r 7 setgoal_recursive

	mkdir -p setgoal_incdec
	for goal in {1..9}; do
		touch setgoal_incdec/setgoal$goal
		saunafs setgoal $goal setgoal_incdec/setgoal$goal
	done
}

metadata_generate_settrashtime() {
	for trashtime in 123 234 345 ; do
		touch settrashtime$trashtime
		saunafs settrashtime $trashtime settrashtime$trashtime
	done
	mkdir -p settrashtime_recursive/dir{1,2}
	chmod 777 settrashtime_recursive/dir{1,2}
	touch settrashtime_recursive/dir{1,2}/file1
	sudo -HEnu saunafstest_2 touch settrashtime_recursive/dir{1,2}/file2
	saunafs settrashtime -r 123456 settrashtime_recursive

	mkdir -p settrashtime_incdec
	saunafs settrashtime 100 settrashtime_incdec
	for i in 100 150 200 250 300; do
		touch settrashtime_incdec/file$i
		saunafs settrashtime $i settrashtime_incdec/file$i
	done
	saunafs settrashtime -r 200+ settrashtime_incdec
	saunafs settrashtime -r 200- settrashtime_incdec
}

metadata_generate_seteattr() {
	touch seteattr_dir{1,2,3,4}
	saunafs seteattr -f noowner seteattr_dir1
	saunafs seteattr -f noattrcache seteattr_dir2
	saunafs seteattr -f noentrycache seteattr_dir3

	mkdir -p seteattr_recursive/dir{1,2}
	chmod 777 seteattr_recursive/dir{1,2}
	touch seteattr_recursive/dir{1,2}/file1
	sudo -HEnu saunafstest_2 touch seteattr_recursive/dir{1,2}/file2
	saunafs seteattr -r -f noowner seteattr_recursive
}

metadata_generate_chunks() {
	# create few nonempty files and chunks
	echo xxxx >chunk_x
	echo yyyy >chunk_y
	echo zzzz >chunk_z
	# test sharing and "unsharing" chunks
	saunafs appendchunks chunk_xyz chunk_x chunk_y chunk_z
	truncate -s2 chunk_x
	echo 'zZzZ' >>chunk_z
}

metadata_generate_chunks_with_goals() {
	for i in {1..20}; do
		mkdir chunks_with_goals_$i
		saunafs setgoal $i chunks_with_goals_$i
		echo a | tee chunks_with_goals_$i/{1..3} >/dev/null
	done
}

metadata_generate_snapshot() {
	# Create a complicated directory tree and make a snapshot of this tree
	mkdir dir_snapshot
	chmod 777 dir_snapshot
	echo abcd | tee dir_snapshot/file1 dir_snapshot/file2 >/dev/null
	ln -s file1 dir_snapshot/symlink
	sudo -HEnu saunafstest_2 touch dir_snapshot/file_3
	sudo -HEnu saunafstest_3 bash -c 'echo xyz > dir_snapshot/file_5'
	mkdir dir_snapshot/level_2
	touch dir_snapshot/level_2/file4
	saunafs makesnapshot dir_snapshot dir_snapshot_s1

	# Test overwriting shared data
	echo aaaaaaaaaaaa > snapshot_file
	saunafs makesnapshot snapshot_file snapshot_file_s1
	saunafs makesnapshot snapshot_file snapshot_file_s2
	echo bbb >> snapshot_file_s1
	truncate -s 1 snapshot_file_s2

	# Test snapshot -o
	mkdir -p dir_snapshot_2
	touch dir_snapshot_2/file4
	saunafs makesnapshot -o dir_snapshot/level_2/file4 dir_snapshot_2/
	saunafs makesnapshot dir_snapshot dir_snapshot_s2
	saunafs makesnapshot -o dir_snapshot dir_snapshot_s2
}

metadata_generate_xattrs() {
	assert_program_installed attr
	touch xattr_file
	mkdir xattr_dir
	attr -qs name1 -V value1 xattr_file
	attr -qs name2 -V value2 xattr_file
	attr -qs name3 -V value3 xattr_dir
	attr -r name1 xattr_file
}

metadata_generate_acls() {
	mkdir acldir
	setfacl -d -m group:fuse:rw- acldir
	setfacl -d -m user:saunafstest:rwx acldir
	touch acldir/aclfile
	touch acldir/aclfile2
	mkdir acldir/aclsubdir
	touch acldir/aclsubdir/aclfile3
	setfacl -m group::r-x acldir/aclfile
	setfacl -x group:fuse acldir/aclfile
	setfacl -m group:root:-wx acldir/aclfile
	setfacl -k acldir
	setfacl -d -m group:fuse:rwx acldir
}

metadata_generate_renames() {
	mkdir rename_dir1
	mkdir rename_dir2
	touch rename_1 rename_2 rename_dir1/rename_3
	mv rename_2 rename_22
	mv rename_1 rename_dir2/
	mv rename_dir1 rename_dir2/
}

metadata_generate_uids_gids() {
	sudo -HEnu saunafstest_2 bash -c 'head -c 12345678 /dev/zero > uidgid_file1'
	sudo -HEnu saunafstest_2 bash -c 'head -c 12876543 /dev/zero > uidgid_file2'
	sudo -HEnu saunafstest_2 sg saunafstest -c 'head -c 23456789 /dev/zero > uidgid_file3'
	sudo -HEnu saunafstest_2 chgrp saunafstest uidgid_file2
	sudo -HEnu saunafstest_3 bash -c 'head -c 13579000 /dev/zero > uidgid_file4'

	sudo -HEnu saunafstest_1 sg saunafstest -c 'mkdir uidgid_dir1'
	sudo -HEnu saunafstest_1 chmod 2775 uidgid_dir1
	sudo -HEnu saunafstest_2 bash -c 'head -c 3456789 /dev/zero > uidgid_dir1/file1'
	sudo -HEnu saunafstest_3 bash -c 'head -c 4567890 /dev/zero > uidgid_dir1/file2'
}

metadata_generate_touch() {
	touch touch_file{1,2,3}
	sleep 1.2
	touch touch_file1
	touch --date='2000-01-01 12:34:56' touch_file2
	touch --date='2001-01-01 13:44:31' touch_file3
	touch -m --date='2002-01-01 12:13:14' touch_file3
}

metadata_generate_truncate() {
	truncate -s 10G truncate_10G
	truncate -s 10000000   truncate_big
	truncate -s 2000000000 truncate_big
	truncate -s 1000000000 truncate_big
	echo -n abcdefghijk | tee truncate_short{1,2,3,4} >/dev/null
	truncate -s 5 truncate_short1
	truncate -s 99 truncate_short2
	truncate -s 0 truncate_short3
	truncate -s "$SAUNAFS_CHUNK_SIZE" truncate_short4
}

metadata_generate_filelocks() {
	metadata_generate_filelocks_ flockcmd r w
	metadata_generate_filelocks_ posixlockcmd w r
}

metadata_generate_filelocks_() {
	for i in {1..6}; do
		touch lock_file$i
		${1} lock_file$i $2 >/dev/null &
		pid1[$i]=$!
	done

	for i in {1..6}; do
		${1} lock_file$i $3 >/dev/null &
		pid2[$i]=$!
	done

	for i in {1..2}; do
		kill -s SIGUSR1 ${pid1[$i]}
	done
	for i in {3..4}; do
		kill -s SIGKILL ${pid1[$i]}
	done

	for i in {1..2}; do
		kill -s SIGUSR1 ${pid2[$i]}
	done
	for i in {3..4}; do
		kill -s SIGKILL ${pid2[$i]}
	done
}

metadata_get_all_generators() {
	echo metadata_generate_files
	echo metadata_generate_funny_inodes
	echo metadata_generate_quotas
	echo metadata_generate_unlink
	echo metadata_generate_trash_ops
	echo metadata_generate_setgoal
	echo metadata_generate_settrashtime
	echo metadata_generate_seteattr
	echo metadata_generate_chunks
	echo metadata_generate_chunks_with_goals
	echo metadata_generate_snapshot
	echo metadata_generate_xattrs
	echo metadata_generate_acls
	echo metadata_generate_renames
	echo metadata_generate_uids_gids
	echo metadata_generate_touch
	echo metadata_generate_truncate
	echo metadata_generate_filelocks
}

metadata_generate_all() {
	for generator in $(metadata_get_all_generators); do
		eval "$generator"
	done
}

# generate_mock_chunks_changelog <chunk_count> [chunks_per_file] [start_version]
# Prints a changelog which populates a filesystem containing no files yet
# with <chunk_count> chunks, spread over files of <chunks_per_file> chunks each.
# Chunk ids are sequential starting from 1 and get version 1 -- matching the
# chunks synthesized by the mock disk plugin (src/chunkserver/plugins/mockdisk),
# so mock chunkservers register exactly the chunks this metadata references.
# All files have the default goal (1): give each mock chunkserver a disjoint
# id range (mock:<count>:<firstChunkId>:...) so every chunk has exactly one
# copy. sfsmetarestore knows no custom goal configuration, so changelogs with
# SETGOAL to other goal ids cannot be applied offline.
# [start_version] must be the current metadata version (metadata_get_version);
# session bookkeeping bumps it above 1 even on a fresh cluster.
# Apply with: sfsmetarestore -a -d <master_data_path> (master stopped).
generate_mock_chunks_changelog() {
	local chunk_count=$1
	local chunks_per_file=${2:-10000}
	local start_version=${3:-1}
	assert_program_installed awk
	awk -v chunk_count="$chunk_count" \
			-v chunks_per_file="$chunks_per_file" \
			-v start_version="$start_version" '
	END {
		version = start_version
		chunkid = 1
		inode = 2
		file_nr = 0
		while (chunkid <= chunk_count) {
			chunks_in_file = chunk_count - chunkid + 1
			if (chunks_in_file > chunks_per_file) {
				chunks_in_file = chunks_per_file
			}
			printf "%d: 1|CREATE(1,mock_%07d,f,420,9,9,0):%d\n", \
					version++, file_nr, inode
			for (i = 0; i < chunks_in_file; ++i) {
				printf "%d: 1|WRITE(%d,%d,1,1):%d\n", version++, inode, i, chunkid++
			}
			printf "%d: 1|LENGTH(%d,%.0f,0)\n", \
					version++, inode, chunks_in_file * 67108864
			++inode
			++file_nr
		}
	}' < /dev/null
}

# Prints long changelog full of create operations, working only on an empty filesystem.
# This changelog applies only to an empty metadata file (version == 1)
generate_changelog() {
	assert_program_installed awk
	awk '
	END {
		version=1
		print("1: 1|SESSION():1")
		for (i = 2; i<2000000; ++i) {
			print(i": 1|CREATE(1,f"i",f,420,9,9,0):"i)
		}
	}' < /dev/null
}

# get_changes <dir> -- prints all the changes that can be found in changelog in the given directory
get_changes() {
	find "$1" -regextype posix-egrep -regex '.*/changelog.sfs([.][0-9]+)?$' | xargs sort -n -u
}

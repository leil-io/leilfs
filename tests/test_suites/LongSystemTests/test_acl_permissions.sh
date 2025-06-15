timeout_set '4 minutes'
assert_program_installed setfacl getfacl python3
touch "$TEMP_DIR/f"
MESSAGE="Testing ACL support in $TEMP_DIR/" assert_success setfacl -m group:fuse:rw "$TEMP_DIR/f"

CHUNKSERVERS=1 \
	USE_RAMDISK=YES \
	SAFS_MOUNT_COMMAND=sfsmount \
	SFSEXPORTS_EXTRA_OPTIONS=nomasterpermcheck,ignoregid \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER,readbuffersexpirationtime=100,maxreadaheadrequests=1,readworkers=10" \
	setup_local_empty_saunafs info

saudir="${info[mount0]}/subdir"
tmpdir="$TEMP_DIR/subdir"
mkdir -p "$saudir" "$tmpdir"
chmod 770 "$saudir" "$tmpdir"

# Do the same things in two trees (saudir and tmpdir) and compare permissions after each command
counter=0
while read command; do
	command=$(sed -e 's/ *#.*//' <<< "$command") # Strip the trailing comment
	[[ $command ]] || continue
	echo "$command"
	export MESSAGE="Executing '$command' in both directory trees"
	( cd "$tmpdir" ; assertlocal_success eval "$command" )
	( cd "$saudir" ; assertlocal_success eval "$command" )

	if [[ $((RANDOM % 10)) == 0 && $counter < 3 ]]; then
		saunafs_master_daemon restart
		saunafs_wait_for_all_ready_chunkservers
		counter=$((counter + 1))
	fi

	cd "$saudir"
	find . 2>/dev/null | while read f; do
		for user in saunafstest saunafstest_{0..3}; do
			export MESSAGE="Veryfing permissions on $f as $user after '$command'"
			expected_perm=$(describe_permissions "$user" "$tmpdir/$f")
			actual_perm=$(describe_permissions "$user" "$saudir/$f")
			expect_equals "$(ls $saudir)" "$(ls $tmpdir)"
			expect_equals "$expected_perm" "$actual_perm"
		done
	done
done <<-'END'
	touch file
	chmod 600 file
	setfacl -m user:saunafstest_1:rwx file
	setfacl -m group:saunafstest_2:r-- file
	setfacl -m group::-w- file
	ln -s file symlink
	sudo -nu saunafstest_1 rm -f file

	mkdir dir1
	chmod 700 dir1
	setfacl -m user:saunafstest_1:rwx dir1
	setfacl -d -m group:saunafstest:r-x dir1
	sudo -nu saunafstest_1 mkdir dir1/dir2
	setfacl -m group:saunafstest:r-x dir1

	sudo -nu saunafstest_0 touch file1           # Create some file as saunafstest_0
	sudo -nu saunafstest_0 chmod 600 file1       # Remove access for all other users
	sudo -nu saunafstest_0 setfacl -m mask::rwx file1   # But make it possible to grant anything
	sudo -nu saunafstest_0 setfacl -m group:saunafstest:r-x file1  # And now add some permissions
	sudo -nu saunafstest_0 setfacl -m user:saunafstest:rw- file1
	sudo -nu saunafstest_0 setfacl -m user:saunafstest_1:-w- file1
	sudo -nu saunafstest_0 setfacl -m user:saunafstest_2:-wx file1

	touch dir1/file2                              # This file will inherit group:saunafstest:r-x
	setfacl -m mask::rwx dir1/file2
	setfacl -m user:saunafstest_0:rwx dir1/file2
	setfacl -m user:saunafstest_2:--- dir1/file2 # Revoke all permissions
	setfacl -m group:saunafstest:--- dir1/file2
	setfacl -m group:saunafstest_1:r-- dir1/file2

	sudo -nu saunafstest_1 cp -a dir1 dir3       # Copy some tree
	sudo -nu saunafstest_1 cp -a dir3 dir1       # Copy some files into a directory with defaults
END

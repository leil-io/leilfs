timeout_set 1 minute
assert_program_installed attr

CHUNKSERVERS=1 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

verify_batch_files() {
	local prefix="$1"
	for i in $(seq 1 500); do
		listed=$(attr -ql "batch_$i")
		if [[ "$listed" != "battr" ]]; then
			test_add_failure "${prefix} listing mismatch on batch_$i: expected 'battr', got '$listed'"
			break
		fi
		actual=$(attr -qg "battr" "batch_$i")
		if [[ "$actual" != "bval_$i" ]]; then
			test_add_failure "${prefix} value mismatch on batch_$i: expected 'bval_$i', got '$actual'"
			break
		fi
	done
}

cd "${info[mount0]}"

# --- Phase 1: Basic xattr persistence through master restart ---

mkdir dir1
echo "content_a" > file_a
echo "content_b" > file_b
echo "content_c" > dir1/file_c

# Set multiple xattrs on file_a
expect_success attr -qs "attr1" -V "value_a1" file_a
expect_success attr -qs "attr2" -V "value_a2" file_a

# Set single xattr on file_b and file_c
expect_success attr -qs "attr1" -V "value_b1" file_b
expect_success attr -qs "attr1" -V "value_c1" dir1/file_c

# Set xattr on directory
expect_success attr -qs "dir_attr" -V "dir_value" dir1

# Verify listing (exercises xattrInodeHash lookup)
expect_equals "$(echo -e "attr1\nattr2" | sort)" "$(attr -ql file_a | sort)"
expect_equals "attr1" "$(attr -ql file_b)"
expect_equals "attr1" "$(attr -ql dir1/file_c)"
expect_equals "dir_attr" "$(attr -ql dir1)"

# Verify getting (exercises xattrDataHash lookup)
expect_equals "value_a1" "$(attr -qg attr1 file_a)"
expect_equals "value_a2" "$(attr -qg attr2 file_a)"
expect_equals "value_b1" "$(attr -qg attr1 file_b)"
expect_equals "value_c1" "$(attr -qg attr1 dir1/file_c)"
expect_equals "dir_value" "$(attr -qg dir_attr dir1)"

# Non-existing xattr should fail
expect_failure attr -qg "nonexistent" file_a
expect_failure attr -qg "attr2" file_b

for i in $(seq 1 500); do
    echo "data" > "batch_$i"
    expect_success attr -qs "battr" -V "bval_$i" "batch_$i"
done

# Verify listing and getting for all batch files before restart
verify_batch_files "Pre-restart"

# Restart master triggers metadata save + load cycle
saunafs_master_daemon restart

# Verify named files survived restart
expect_equals "$(echo -e "attr1\nattr2" | sort)" "$(attr -ql file_a | sort)"
expect_equals "attr1" "$(attr -ql file_b)"
expect_equals "value_a1" "$(attr -qg attr1 file_a)"
expect_equals "value_a2" "$(attr -qg attr2 file_a)"
expect_equals "value_b1" "$(attr -qg attr1 file_b)"
expect_equals "value_c1" "$(attr -qg attr1 dir1/file_c)"
expect_equals "dir_value" "$(attr -qg dir_attr dir1)"
expect_failure attr -qg "nonexistent" file_a

# Verify batch files survived restart (listing + getting)
verify_batch_files "Post-restart"

# --- Phase 2: Remove all xattrs from a file ---

expect_success attr -qr "attr1" file_a
expect_success attr -qr "attr2" file_a

# Xattrs should be gone from file_a
expect_failure attr -qg "attr1" file_a
expect_failure attr -qg "attr2" file_a

# Other files should be unaffected
expect_equals "value_b1" "$(attr -qg attr1 file_b)"
expect_equals "value_c1" "$(attr -qg attr1 dir1/file_c)"
expect_equals "dir_value" "$(attr -qg dir_attr dir1)"

# Persist and verify
saunafs_master_daemon restart

expect_failure attr -qg "attr1" file_a
expect_failure attr -qg "attr2" file_a
expect_equals "value_b1" "$(attr -qg attr1 file_b)"
expect_equals "value_c1" "$(attr -qg attr1 dir1/file_c)"
expect_equals "dir_value" "$(attr -qg dir_attr dir1)"

# --- Phase 3: Remove all files that have xattrs ---

rm file_b
rm dir1/file_c
expect_success attr -qr "dir_attr" dir1

# Persist and verify system stability
saunafs_master_daemon restart

# Deleted files should not exist
expect_failure test -e file_b
expect_failure test -e dir1/file_c
# Directory xattr should be gone
expect_failure attr -qg "dir_attr" dir1

# --- Phase 4: New xattrs work after clearing everything ---

echo "new" > fresh_file
expect_success attr -qs "fresh_attr" -V "fresh_value" fresh_file
expect_equals "fresh_attr" "$(attr -ql fresh_file)"
expect_equals "fresh_value" "$(attr -qg fresh_attr fresh_file)"

saunafs_master_daemon restart

expect_equals "fresh_attr" "$(attr -ql fresh_file)"
expect_equals "fresh_value" "$(attr -qg fresh_attr fresh_file)"

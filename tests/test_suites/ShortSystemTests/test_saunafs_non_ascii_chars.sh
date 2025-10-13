USE_RAMDISK="YES" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

# Non-ASCII test names
polish="zażółć"
chinese="文件夹"
arabic="ملف"

# Create directories
mkdir "${polish}" "${chinese}" "${arabic}"

# Create files inside each directory
touch "${polish}/${polish}_plik"
touch "${chinese}/${chinese}_file"
touch "${arabic}/${arabic}_ملف"

# Run saunafs dirinfo on each directory
for dir in "${polish}" "${chinese}" "${arabic}"; do
    echo "Testing dirinfo for ${dir}"
    assert_success saunafs_command dirinfo "${dir}"
done

# Run saunafs fileinfo on each file
assert_success saunafs_command fileinfo "${polish}/${polish}_plik"
assert_success saunafs_command fileinfo "${chinese}/${chinese}_file"
assert_success saunafs_command fileinfo "${arabic}/${arabic}_ملف"

# Clean up
rm "${polish}/${polish}_plik"
rm "${chinese}/${chinese}_file"
rm "${arabic}/${arabic}_ملف"
rmdir "${polish}" "${chinese}" "${arabic}"

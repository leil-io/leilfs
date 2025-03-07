MOUNTS=4 \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

mounts=$(saunafs_admin_command list-mounts --porcelain --verbose localhost "${info[matocl]}")
expect_equals "4" $(wc -l <<< "$mounts")
for i in {1..4}; do
	if is_windows_system; then
		expect_equals \
			"$i $(echo -n $i | tr '[1-9]' '[F-N]'): $SAUNAFS_VERSION / 0 0 999 999 no yes no no no 1 40 - -" \
			"$(sed -n "${i}p" <<< "$mounts" | cut -d' ' -f 1,3-)"
	else
		expect_equals \
			"$i ${info[mount$((i - 1))]} $SAUNAFS_VERSION / 0 0 999 999 no yes no no no 1 40 - -" \
			"$(sed -n "${i}p" <<< "$mounts" | cut -d' ' -f 1,3-)"
	fi
done

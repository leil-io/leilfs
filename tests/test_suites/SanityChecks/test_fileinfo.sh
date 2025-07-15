awkscript='
/:$/ { next }   # skip filenames

/^\tchunk/ {
	chunkid = $3
	dir = substr(chunkid, 11, 2)
	next
}
!/^\t\tcopy/ {
	printf("UNKNOWN LINE: %s\n", $0)
	exit
}
/part [1-9]\/[2-9] of ec\(2,1\)/ {
	split($3, server, ":")
	printf "CS%s/chunks%s/chunk_ec2_%d_of_2_1_%s%s\n", server[2], dir, $5, chunkid, meta_extension
	next
}
/part [1-9]\/[2-9] of ec\(3,1\)/ {
	split($3, server, ":")
	printf "CS%s/chunks%s/chunk_ec2_%d_of_3_1_%s%s\n", server[2], dir, $5, chunkid, meta_extension
	next
}
/part [1-9]\/[2-9] of ec\(3,2\)/ {
	split($3, server, ":")
	printf "CS%s/chunks%s/chunk_ec2_%d_of_3_2_%s%s\n", server[2], dir, $5, chunkid, meta_extension
	next
}
{
	split($3, server, ":")
	printf "CS%s/chunks%s/chunk_%s%s\n", server[2], dir, chunkid, meta_extension
	next
}
'

CHUNKSERVERS=5 \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	MASTER_CUSTOM_GOALS="20 ec31: \$ec(3,1)" \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

files=()
for goal in 1 2 3 ec21 ec31 ec32; do
	file="file_goal_$goal"
	touch "$file"
	saunafs_command setgoal "$goal" "$file"
	dd if=/dev/zero of="$file" bs=1MiB count=5 seek=62 conv=notrunc
	truncate -s 100M "$file" # Increases version of the second chunk
	files+=("$file")
done

chunks_info=$(saunafs_command fileinfo "${files[@]}" \
		| awk -v meta_extension="${chunk_metadata_extension}" "$awkscript" \
		| sed -e "s|CS${info[chunkserver0_port]}|$(get_metadata_path ${info[chunkserver0_hdd]})|" \
		| sed -e "s|CS${info[chunkserver1_port]}|$(get_metadata_path ${info[chunkserver1_hdd]})|" \
		| sed -e "s|CS${info[chunkserver2_port]}|$(get_metadata_path ${info[chunkserver2_hdd]})|" \
		| sed -e "s|CS${info[chunkserver3_port]}|$(get_metadata_path ${info[chunkserver3_hdd]})|" \
		| sed -e "s|CS${info[chunkserver4_port]}|$(get_metadata_path ${info[chunkserver4_hdd]})|" \
		| sort)

chunks_real=$(find_all_metadata_chunks | sort)

expect_equals "$chunks_real" "$chunks_info"

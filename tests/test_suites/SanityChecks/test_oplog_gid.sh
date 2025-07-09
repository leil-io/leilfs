CHUNKSERVERS=1 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER="NO" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

# Redirect the oplog to a file in the background
oplog_output_file="${TEMP_DIR}/oplog.log"
timeout 3 sudo cat .oplog > "${oplog_output_file}" &

# Do some operations to generate oplog output
mkdir dir
touch dir/file
echo "saunafs" > dir/file
cat dir/file > /dev/null
truncate -s 3 dir/file
mv dir/file dir/file2
cp dir/file2 dir/file3
ls -l dir/ > /dev/null

# Wait for the timeout to finish the process running in background
wait

# The gid is encoded in some operations by setting the most significative bit
# like this:
# constexpr std::uint32_t kSecondaryGroupsBit = (std::uint32_t)1 << 31;
# In this scenario the oplog should look into the context's gids to print the
# primary gid for the user and not the encoded flag.
secondaryGroupsFlag=2147483650

gidCount=$(grep "gid:" "${oplog_output_file}" | grep -v "internal node" | wc -l)
badGidCount=$(grep "gid:${secondaryGroupsFlag}" "${oplog_output_file}" | wc -l)

currentUserGid=$(id -g)
goodGidCount=$(grep "gid:${currentUserGid}" "${oplog_output_file}" | wc -l)

echo "Total gid appearances: ${gidCount}"
echo "Good gid appearances:  ${goodGidCount}"
echo "Bad gid appearances:    ${badGidCount}"

assert_equals 0 ${badGidCount}

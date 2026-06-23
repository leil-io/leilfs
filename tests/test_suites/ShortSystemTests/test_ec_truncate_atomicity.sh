# FDB-backed masters may spend extra time replaying commit conflicts in this
# many-writers-plus-truncates-on-one-file workload. The in-memory master keeps
# the historical timeout.
if [[ "${METADATA_BACKEND:-}" == "FDB" ]]; then
	timeout_set '3 minutes'
else
	timeout_set '1 minute'
fi

CHUNKSERVERS=11 \
	MASTER_CUSTOM_GOALS="3 ec32: \$ec(3,2)`
	`|4 ec42: \$ec(4,2)`
	`|7 ec72: \$ec(7,2)`
	`|9 ec92: \$ec(9,2)" \
	DISK_PER_CHUNKSERVER=1 \
	MOUNTS=5 \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

# Create a source file -- a valid file-generated file which consists of 200 kB of data
source="$RAMDISK_DIR/source"
FILE_SIZE=200K file-generate "$source"

cd "${info[mount0]}"
for ec in 3 4 7 9; do
	# Create a file which consists of 400 kB of random data
	file="file$ec"
	touch "$file"
	saunafs setgoal ec${ec}2 "$file"
	head -c 400K /dev/urandom > "$file"

	# Run in parallel 200 dd processes, each copies different 1 kB of data from the source file
	# Distribute these processes between 3 first mountpoints (mount0, mount1, mount2)
	for i in {0..199}; do
		( sleep 0.$((2 * i + 100)) && dd if="$source" of="${info[mount$((i%3))]}/$file" \
				bs=1KiB skip=$i seek=$i count=1 conv=notrunc 2>/dev/null ) &
	done
	# In the meanwhile, shorten file from 400K to 200K, chopping 1 kB in each of 200 steps
	# Sometimes share mountpoint with dd (mount2), sometimes not (mount3, mount4)
	for i in {399..200}; do
		truncate -s ${i}K "${info[mount$((2 + i % 3))]}/$file"
	done
	wait # Wait for all dd processes to finish

	# Now file should be equal to the source file. Let's validate it!
	MESSAGE="Testing ec(${ec},2)" assert_success file-validate "$file"
done

# Remove data parts 1 and 2 of each chunk to verify parity parts
find_all_chunks -name "*ec2_1_of*" | xargs rm -vf
find_all_chunks -name "*ec2_2_of*" | xargs rm -vf
MESSAGE="Verification of parity parts" expect_success file-validate file*

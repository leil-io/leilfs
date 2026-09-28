# Once a write rejected by the quota is closed, the file accepts a truncate
# and a new write again, however often the rejection repeats.
timeout_set 3 minutes

iterations=20

# A small write cache per inode keeps the writer waiting for free cache while
# the quota rejects its data.
USE_RAMDISK=YES \
	SFSEXPORTS_EXTRA_OPTIONS="allcanchangequota" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER|sfsmaxchunkswritteninparallelperinode=1|`
		`sfswritecachesize=16|sfscacheperinodepercentage=10" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

mkdir dir
head -c 1024 /dev/zero > dir/file
one_kb_file_size=$(sfs_dir_info size dir/file)
# The file's single chunk fills the quota, so any new chunk is rejected
saunafs setquota -d $one_kb_file_size $one_kb_file_size 0 0 dir

for ((i = 1; i <= iterations; i++)); do
	# The second chunk is over the quota
	expect_failure dd if=/dev/zero of=dir/file bs=1M seek=64 count=8 conv=notrunc
	# The rejected handle is released asynchronously, so the truncate may need
	# a few tries, but it must succeed
	assert_eventually 'truncate -s 512 dir/file' '15 seconds'
	assert_equals 512 "$(stat --format=%s dir/file)"
	# Rewriting the existing chunk is always allowed
	assert_success dd if=/dev/zero of=dir/file bs=1k count=1 conv=notrunc
	assert_equals 1024 "$(stat --format=%s dir/file)"
done

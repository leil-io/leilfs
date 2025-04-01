timeout_set 2 minutes

# Returns number of standard chunks and number of different parts for each ec level.
# If the same chunk has two copies of the same parts, these will be counted as one, eg:
# '3 standard' -- just 3 standard chunks
# '2 standard 3 ec2' -- 2 standard chunks and 3 ec2 chunks
# '6 ec2' -- 6 ec2 chunks (each is different)
chunks_state() {
	{
		find_all_metadata_chunks | grep -o chunk_.* | grep -o chunk_00000 | sed -e 's/.*/standard/'
		find_all_metadata_chunks | grep -o chunk_.* | sort -u | grep -o '_of_[2-9]_1' | sed -e 's/_of_/ec2_/'
	} | sort | uniq -c | tr '\n' ' ' | trim_hard
}

count_chunks_on_chunkservers() {
	for i in $@; do
		find_chunkserver_metadata_chunks $i
	done | wc -l
}

USE_RAMDISK=YES \
	CHUNKSERVERS=9 \
	CHUNKSERVER_LABELS="3,4,5:hdd|6,7,8:ssd" \
	MASTER_CUSTOM_GOALS="10 ec21_ssd: \$ec(2,1) {ssd ssd ssd}`
			`|11 ec31_hdd: \$ec(3,1) {hdd hdd hdd hdd}`
			`|12 ec51_mix: \$ec(5,1) {hdd ssd hdd ssd}" \
	MASTER_EXTRA_CONFIG="CHUNKS_LOOP_MIN_TIME = 1`
			`|CHUNKS_LOOP_MAX_CPU = 90`
			`|CHUNKS_SOFT_DEL_LIMIT = 10`
			`|CHUNKS_WRITE_REP_LIMIT = 10`
			`|OPERATIONS_DELAY_INIT = 0`
			`|CHUNKS_REBALANCING_BETWEEN_LABELS=1`
			`|OPERATIONS_DELAY_DISCONNECT = 0"\
	setup_local_empty_saunafs info

cd "${info[mount0]}"
mkdir dir
saunafs setgoal ec21_ssd dir
FILE_SIZE=1K file-generate dir/file

assert_equals "3 ec2_2_1" "$(chunks_state)"
assert_equals 3 "$(count_chunks_on_chunkservers {6..8})"
assert_equals 3 "$(count_chunks_on_chunkservers {0..8})"

saunafs setgoal ec31_hdd dir/file
#Wait for migration to finish
assert_eventually_prints '4 ec2_3_1' 'chunks_state' '2 minutes'
assert_equals 3 "$(count_chunks_on_chunkservers {3..5})"
assert_equals 4 "$(count_chunks_on_chunkservers {0..8})"

saunafs setgoal ec51_mix dir/file
#Wait for migration to finish
assert_eventually_prints '6 ec2_5_1' 'chunks_state' '2 minutes'
assert_less_or_equal 2 "$(count_chunks_on_chunkservers {3..5})"
assert_less_or_equal 2 "$(count_chunks_on_chunkservers {6..8})"

# On the FDB backend each snapshot below waits for a chunk health measurement that covers the
# cluster as it is at that point. The in-memory master keeps the historical timeout.
if [[ "${METADATA_BACKEND:-}" == "FDB" ]]; then
	timeout_set '2 minutes'
else
	timeout_set '1 minute'
fi

CHUNKSERVERS=4 \
	MASTER_EXTRA_CONFIG="OPERATIONS_DELAY_INIT = 100000" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

# The measurement row moves by design on a backend that measures in the background; the counts
# are what this test compares. A backend whose counters are current prints no such row.
chunks_health_counts() {
	saunafs-admin chunks-health --porcelain localhost "${info[matocl]}" | grep -v "^MEA "
}

# Each snapshot is taken once the report covers the cluster as it is now: a measuring backend
# answers with its last measurement, so reading it right after a chunkserver change would compare
# the new cluster against the old answer. A no-op where counters are current.

# Create one chunk in each of four goals
goals="2 3 xor2 xor3"
cd "${info[mount0]}"
for goal in $goals; do
	mkdir dir_$goal
	saunafs setgoal $goal dir_$goal
	echo a > dir_$goal/file
done

export MESSAGE="Veryfing health report with all the chunkservers up"
wait_for_chunk_health_measurement "${info[matocl]}"
health4=$(chunks_health_counts)
expect_equals 4 $(awk '/AVA/ {chunks += ($3 + $4 + $5)} END {print chunks}' <<< "$health4")
for goal in $goals; do
	expect_awk_finds "/AVA $goal 1 0 0/" "$health4"
done
expect_awk_finds_no '/AVA/ && $4 > 0' "$health4"
expect_awk_finds_no '/AVA/ && $5 > 0' "$health4"
expect_awk_finds_no '/DEL [xor0-9]+ [0-9]+ .*[1-9]/' "$health4"
expect_awk_finds_no '/REP [xor0-9]+ [0-9]+ .*[1-9]/' "$health4"

# Choose chunkservers to stop in the way, that the chunkserver with one copy
# of the chunk in goal 2 will be turned off as the first one
chunk=$(saunafs fileinfo dir_2/file | awk '/chunk 0:/{print $3}')
csid1=$(find_first_chunkserver_with_chunks_matching "*$chunk*")
csid2=$(( (csid1 + 1) % 4 ))
csid3=$(( (csid2 + 1) % 4 ))
csid4=$(( (csid3 + 1) % 4 ))

export MESSAGE="Veryfing health report one chunkserver down and chunk with goal 2 endangered"
saunafs_chunkserver_daemon $csid1 stop
saunafs_wait_for_ready_chunkservers 3
wait_for_chunk_health_measurement "${info[matocl]}"
health3=$(chunks_health_counts)
expect_equals 4 $(awk '/AVA/ {chunks += ($3 + $4 + $5)} END {print chunks}' <<< "$health3")
expect_awk_finds '/AVA 2 0 1 0/' "$health3"
expect_awk_finds '/AVA xor3 0 1 0/' "$health3"
expect_awk_finds_no '/AVA/ && $5 > 0' "$health3"
expect_awk_finds_no '/AVA 3/ && $4 > 0' "$health3"
expect_awk_finds '/REP 2 0 1 [ 0]+$/' "$health3"
expect_awk_finds '/REP xor3 0 1 [ 0]+$/' "$health3"
expect_awk_finds_no '/DEL [xor0-9]+ [0-9]+ .*[1-9]/' "$health3"

export MESSAGE="Veryfing health report with two out of four chunkservers down"
saunafs_chunkserver_daemon $csid2 stop
saunafs_wait_for_ready_chunkservers 2
wait_for_chunk_health_measurement "${info[matocl]}"
health2=$(chunks_health_counts)
expect_equals 4 $(awk '/AVA/ {chunks += ($3 + $4 + $5)} END {print chunks}' <<< "$health2")
expect_awk_finds '/AVA xor3 0 0 1/' "$health2"
expect_awk_finds_no '/REP xor/ && $3 > 0' "$health2"
expect_awk_finds '/REP 2 0 (0 1|1 0) [ 0]+$/' "$health2"
expect_awk_finds '/REP xor3 0 0 1 [ 0]+$/' "$health2"
expect_awk_finds_no '/DEL [xor0-9]+ [0-9]+ .*[1-9]/' "$health2"

export MESSAGE="Veryfing health report with three out of four chunkservers down"
saunafs_chunkserver_daemon $csid3 stop
saunafs_wait_for_ready_chunkservers 1
wait_for_chunk_health_measurement "${info[matocl]}"
health1=$(chunks_health_counts)
expect_equals 4 $(awk '/AVA/ {chunks += ($3 + $4 + $5)} END {print chunks}' <<< "$health1")
expect_awk_finds_no '/AVA/ && $3 > 0' "$health1"
expect_awk_finds_no '/AVA xor/ && $4 > 0' "$health1"
expect_awk_finds_no '/REP/ && $3 > 0' "$health1"
expect_awk_finds '/REP 2 0 (0 1|1 0) [ 0]+$/' "$health1"
expect_awk_finds '/REP xor3 0 0 0 1 [ 0]+$/' "$health1"
expect_awk_finds_no '/DEL [xor0-9]+ [0-9]+ .*[1-9]/' "$health1"

export MESSAGE="Veryfing health report with all chunkservers down"
saunafs_chunkserver_daemon $csid4 stop
saunafs_wait_for_ready_chunkservers 0
wait_for_chunk_health_measurement "${info[matocl]}"
health0=$(chunks_health_counts)
expect_equals 4 $(awk '/AVA/ {chunks += ($3 + $4 + $5)} END {print chunks}' <<< "$health0")
expect_awk_finds_no '/AVA/ && $3 > 0' "$health0"
expect_awk_finds_no '/AVA/ && $4 > 0' "$health0"
expect_awk_finds_no '/REP/ && $3 > 0' "$health0"
expect_awk_finds_no '/REP/ && $4 > 0' "$health0"
expect_awk_finds '/REP 2 0 0 1 0 0 /' "$health0"
expect_awk_finds '/REP 3 0 0 0 1 0 /' "$health0"
expect_awk_finds '/REP xor2 0 0 0 1 0 /' "$health0"
expect_awk_finds '/REP xor3 0 0 0 0 1 /' "$health0"
expect_awk_finds_no '/DEL [xor0-9]+ [0-9]+ .*[1-9]/' "$health0"

export MESSAGE="Veryfing health report with one out of four chunkservers up again"
saunafs_chunkserver_daemon $csid4 start
saunafs_wait_for_ready_chunkservers 1
wait_for_chunk_health_measurement "${info[matocl]}"
expect_equals "$health1" "$(chunks_health_counts)"

export MESSAGE="Veryfing health report with two out of four chunkservers up again"
saunafs_chunkserver_daemon $csid3 start
saunafs_wait_for_ready_chunkservers 2
wait_for_chunk_health_measurement "${info[matocl]}"
expect_equals "$health2" "$(chunks_health_counts)"

export MESSAGE="Veryfing health report with three out of four chunkservers up again"
saunafs_chunkserver_daemon $csid2 start
saunafs_wait_for_ready_chunkservers 3
wait_for_chunk_health_measurement "${info[matocl]}"
expect_equals "$health3" "$(chunks_health_counts)"

export MESSAGE="Veryfing health report with all the chunkservers up again"
saunafs_chunkserver_daemon $csid1 start
saunafs_wait_for_ready_chunkservers 4
wait_for_chunk_health_measurement "${info[matocl]}"
expect_equals "$health4" "$(chunks_health_counts)"

timeout_set 2 minutes

# Closing a file must release the inode it was opened on, even when its path no longer leads
# there: the file may have been deleted through the handle being closed, deleted or replaced
# while another handle still holds it, or renamed while open. A mount that missed such files
# would keep them open for good and keep reporting them to the master, which then never frees
# them. The path also stops leading there when its directory is renamed or another mount changes
# the file, and cannot be looked up at all while the master is unreachable. After every case,
# once nothing is held open, no session may have open files.

# Open files of all sessions together, or "unavailable" when the master cannot be asked, so a
# master that is still starting never reads as zero open files.
check_open_files() {
	saunafs-admin list-sessions localhost "${info[matocl]}" 2>/dev/null |
		awk -F': ' '/Open files: / {sum += $2; n++} END {print (n ? sum : "unavailable")}'
}

# Holds the given number of files open in a new directory until stop_holder.
start_holder() {
	local directory=$1
	local count=$2
	local ready_file="${TEMP_DIR}/ready_${directory##*/}"
	hold-open-files "${directory}" "${count}" "${ready_file}" &
	holder_pid=$!
	assert_eventually "test -e '${ready_file}'" '1 minute'
	assert_eventually_prints "${count}" check_open_files '30 seconds'
}

# Closes the files held by start_holder and checks the session has none open afterwards.
stop_holder() {
	kill "${holder_pid}"
	local holder_status=0
	wait "${holder_pid}" || holder_status=$?
	assert_equals 0 "${holder_status}"
	assert_eventually_prints 0 check_open_files '30 seconds'
}

MOUNTS=2 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfsreportreservedperiod=1" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"
other="${info[mount1]}"

held_count=10

# Deleted after being closed.
for ((i = 0; i < 100; i++)); do
	echo "data ${i}" > "deleted_${i}"
done
assert_success rm deleted_*
assert_eventually_prints 0 check_open_files '30 seconds'

# Overwritten: opened again with truncation, several times each.
for ((round = 0; round < 3; round++)); do
	for ((i = 0; i < 50; i++)); do
		echo "round ${round}" > "overwritten_${i}"
	done
done
assert_eventually_prints 0 check_open_files '30 seconds'

# Opened and closed by several processes at once.
parallel_pids=()
for ((job = 0; job < 8; job++)); do
	(
		for ((i = 0; i < 50; i++)); do
			echo "job ${job}" > "parallel_${job}_${i}"
			cat "parallel_${job}_${i}" > /dev/null
		done
	) &
	parallel_pids+=($!)
done
for pid in "${parallel_pids[@]}"; do
	job_status=0
	wait "${pid}" || job_status=$?
	assert_equals 0 "${job_status}"
done
assert_eventually_prints 0 check_open_files '30 seconds'

# Renamed while open.
start_holder renamed "${held_count}"
for ((i = 0; i < held_count; i++)); do
	assert_success mv "renamed/file_${i}" "renamed/moved_${i}"
done
stop_holder

# Deleted while another handle still holds them: they stay open until closed.
start_holder held_deleted "${held_count}"
assert_success rm held_deleted/file_*
assert_eventually_prints "${held_count}" check_open_files '30 seconds'
stop_holder

# Replaced by a rename while held, and renamed over an existing file while held.
start_holder replaced "${held_count}"
for ((i = 0; i < held_count; i++)); do
	echo "new ${i}" > "replaced/source_${i}"
	echo "old ${i}" > "replaced/target_${i}"
done
for ((i = 0; i < held_count / 2; i++)); do
	assert_success mv "replaced/source_${i}" "replaced/file_${i}"
done
for ((i = held_count / 2; i < held_count; i++)); do
	assert_success mv "replaced/file_${i}" "replaced/target_${i}"
done
# The sources and targets written above are released at the next report; the held files stay.
assert_eventually_prints "${held_count}" check_open_files '30 seconds'
stop_holder

# The directory holding the open files renamed through the other mount. The wait lets this
# mount's cached directory entries expire, as they would for a file kept open a while.
start_holder in_directory "${held_count}"
assert_success mv "${other}/in_directory" "${other}/moved_directory"
sleep 2
stop_holder

# Held files deleted and renamed through the other mount.
start_holder changed_elsewhere "${held_count}"
for ((i = 0; i < held_count / 2; i++)); do
	assert_success rm "${other}/changed_elsewhere/file_${i}"
done
for ((i = held_count / 2; i < held_count; i++)); do
	assert_success mv "${other}/changed_elsewhere/file_${i}" "${other}/changed_elsewhere/moved_${i}"
done
stop_holder

# Closed while the master is down: the mount releases them once it is connected again. The shell
# leaves the mount first, as commands started from inside it would wait on the mount for minutes
# while the master is down.
cd "${TEMP_DIR}"
start_holder "${info[mount0]}/master_down" "${held_count}"
saunafs_master_daemon stop
sleep 5
kill "${holder_pid}"
holder_status=0
wait "${holder_pid}" || holder_status=$?
assert_equals 0 "${holder_status}"
saunafs_master_daemon start
assert_eventually_prints 0 check_open_files '1 minute'

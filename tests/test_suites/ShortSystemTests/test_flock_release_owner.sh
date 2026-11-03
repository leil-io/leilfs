timeout_set 2 minutes

# A flock waiter fails with EIO when the master restarts, but its request stays queued there.
# Closing the waiter's file must remove that request, or unlocking the holder would grant the
# lock to a process that already gave up.
CHUNKSERVERS=1 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER|enablefilelocks=1" \
	setup_local_empty_saunafs info

# Number of flock locks the master has in a state: active (granted) or pending (queued).
flock_count() {
	local state="${1}" rows
	rows=$(saunafs_admin_master manage-locks list flock --porcelain "--${state}") || return
	awk '$1 ~ /^[0-9]+$/ {count++} END {print count+0}' <<< "${rows}"
}

# flockcmd w takes an exclusive lock and keeps the file open; SIGINT closes the file and SIGUSR1
# unlocks. LC_ALL=C keeps its error text in English for the EIO check below.
lock_file="${info[mount0]}/lockfile"
touch "${lock_file}"
LC_ALL=C flockcmd "${lock_file}" w > "${TEMP_DIR}/holder.log" 2>&1 &
holder_pid=$!
assert_eventually_prints 1 'flock_count active'
LC_ALL=C flockcmd "${lock_file}" w > "${TEMP_DIR}/waiter.log" 2>&1 &
waiter_pid=$!
assert_eventually_prints 1 'flock_count pending'

# The restart fails the waiting flock() with EIO; the master keeps both locks in its metadata.
saunafs_master_daemon restart
saunafs_wait_for_all_ready_chunkservers
assert_eventually_matches 'flock[(][)] failed: Input/output error' "cat '${TEMP_DIR}/waiter.log'"
assert_eventually_prints 1 'flock_count active'
assert_eventually_prints 1 'flock_count pending'

# Confirm reconnection before attempting the best-effort close release.
assert_eventually '[[ $(stat -f -c %b "${info[mount0]}") -gt 0 ]]'

# Closing the waiter's file removes its queued request; the holder keeps its lock.
kill -s SIGINT "${waiter_pid}"
assert_eventually_prints 0 'flock_count pending'
assert_eventually_prints 1 'flock_count active'

# Unlocking the holder then grants the lock to nobody.
kill -s SIGUSR1 "${holder_pid}"
assert_eventually_prints 0 'flock_count active'

kill -KILL "${holder_pid}" "${waiter_pid}" 2>/dev/null || true
wait "${holder_pid}" "${waiter_pid}" 2>/dev/null || true

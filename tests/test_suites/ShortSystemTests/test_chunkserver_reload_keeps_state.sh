# A reload must keep the disk locks, a valid label over an invalid one, and the
# options in use when the file cannot be read. A size option out of a double's
# range is a parse error, at start and on reload.
CHUNKSERVERS=1 \
	DISK_PER_CHUNKSERVER=2 \
	CHUNKSERVER_LABELS="0:ssd" \
	CHUNKSERVER_EXTRA_CONFIG="HDD_LEAVE_SPACE_DEFAULT = 1e999`
			`|MAGIC_DEBUG_LOG = ${TEMP_DIR}/log|LOG_FLUSH_ON=INFO" \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

cfg="${info[chunkserver0_cfg]}"

# Succeeds if this process can lock the file, so no other process holds it.
can_lock() {
	python3 -c 'import fcntl, sys
fcntl.lockf(open(sys.argv[1], "r+"), fcntl.LOCK_EX | fcntl.LOCK_NB)' "$1" 2>/dev/null
}

assert_disks_locked() {
	for lock_file in ${lock_files}; do
		assert_failure can_lock "${lock_file}"
	done
}

chunkserver_labels() {
	saunafs-admin list-chunkservers --porcelain localhost "${info[matocl]}" | awk '{print $10}'
}

log_count() {
	grep -c "$1" "${TEMP_DIR}/log" || true
}

# Signals the daemon directly: the reload command reads the file as well.
reload_chunkserver() {
	local reloads=$(log_count 'reloading config files')
	pgrep -a sfschunkserver | grep -F "${cfg}" | awk '{print $1}' | xargs -r kill -HUP
	assert_eventually "(( \$(log_count 'reloading config files') > ${reloads} ))"
}

lock_files=$(get_chunkserver_hdds_ 0 | sed -e 's|/*$|/.lock|')
assert_equals 2 "$(wc -l <<< "${lock_files}")"
assert_success saunafs_chunkserver_daemon 0 isalive
assert_eventually "(( \$(log_count 'HDD_LEAVE_SPACE_DEFAULT parse error') > 0 ))"
assert_disks_locked
assert_equals ssd "$(chunkserver_labels)"

# A size too small for a double, and an invalid label.
parse_errors=$(log_count 'HDD_LEAVE_SPACE_DEFAULT parse error')
sed -i -re 's/= 1e999$/= 1e-999/' -e 's/^LABEL ?=.*/LABEL = not valid/' "${cfg}"
reload_chunkserver
assert_eventually "(( \$(log_count 'HDD_LEAVE_SPACE_DEFAULT parse error') > ${parse_errors} ))"
assert_success saunafs_chunkserver_daemon 0 isalive
assert_disks_locked

# A file that cannot be read; a label falling back to its default would reach
# the master within the wait.
chmod 000 "${cfg}"
reload_chunkserver
sleep 3
chmod 644 "${cfg}"
assert_equals ssd "$(chunkserver_labels)"
assert_equals 1 "$(saunafs-admin ready-chunkservers-count localhost "${info[matocl]}")"

# The chunkserver registers its label again when it reconnects.
saunafs_master_daemon restart
assert_eventually_prints 1 "saunafs-admin ready-chunkservers-count localhost ${info[matocl]}"
assert_equals ssd "$(chunkserver_labels)"

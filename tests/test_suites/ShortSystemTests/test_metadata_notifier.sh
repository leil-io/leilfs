timeout_set "1 minute"

# Generate TLS configuration files for tests
# Set up temp directory for certs
echo "Generating TLS certificates for master-notifier communication test..."
TLS_CERTS_DIR=${TEMP_DIR}/testcerts
generate_certs ${TLS_CERTS_DIR}

# Create TLS config file pointing to reload TLS config on notifier side
cat > ${TEMP_DIR}/sfstls.cfg <<EOF
tlscertfile=${TLS_CERTS_DIR}/client.crt
tlskeyfile=${TLS_CERTS_DIR}/client.key
tlsservercacertfile=${TLS_CERTS_DIR}/ca.crt
tlsisserver=false
tlsexpectedhostname=sfsmaster
EOF

# Configure master to use TLS certs
master_cfg="|TLS_CERT_FILE = ${TLS_CERTS_DIR}/server.crt"
master_cfg+="|TLS_KEY_FILE = ${TLS_CERTS_DIR}/server.key"
master_cfg+="|TLS_CA_CERT_FILE = ${TLS_CERTS_DIR}/ca.crt"

# Notifier replies report the current state of an inode, so each reserved file stays open until
# its reply is logged; forced cleanup of reserved files would end that state at a random time.
master_cfg+="|EMPTY_RESERVED_FILES_PERIOD_MSECONDS = 0"

wait_for_notifier_line() {
	local log="${1}"
	local line="${2}"
	assert_eventually "grep -Fxq '${line}' '${log}'"
}

# Unregistered notifier connections report version 0.0.0 and get no events.
count_notifiers() {
	local listing
	listing=$(saunafs_admin_command list-inotifiers --porcelain localhost "${info[matocl]}") \
		|| return 1
	if [[ "${1:-}" == "registered" ]]; then
		awk 'NF && $1 != "0.0.0" {count++} END {print count + 0}' <<< "${listing}"
	else
		awk 'NF {count++} END {print count + 0}' <<< "${listing}"
	fi
}

run_metadata_operations() {
	local log="${1}"
	cd "${info[mount0]}"

	mkdir folder1 folder2
	mkdir folder1/subfolder1 folder1/subfolder2

	local folders=(
		"."
		"folder1"
		"folder2"
		"folder1/subfolder1"
		"folder1/subfolder2"
	)

	for f in "${folders[@]}"; do
		ls "${f}"
		sleep 1
	done

	# Held open through the purge: a purged file without a session is deleted, not reserved.
	local file1_fd file1_inode
	saunafs settrashtime 3 folder1
	touch folder1/file1
	exec {file1_fd}<folder1/file1
	file1_inode=$(inode_of folder1/file1)
	rm folder1/file1
	wait_for_notifier_line "${log}" "inode ${file1_inode}: type=t path=/folder1/file1 (trash)"
	wait_for_notifier_line "${log}" "inode ${file1_inode}: type=r path=/folder1/file1 (reserved)"
	exec {file1_fd}<&-
	wait_for_notifier_line "${log}" "inode ${file1_inode}: type=? path="

	local file2_fd file2_inode
	saunafs settrashtime 0 folder1
	touch folder1/file2
	exec {file2_fd}<folder1/file2
	file2_inode=$(inode_of folder1/file2)
	rm folder1/file2
	wait_for_notifier_line "${log}" "inode ${file2_inode}: type=r path=/folder1/file2 (reserved)"
	exec {file2_fd}<&-
	wait_for_notifier_line "${log}" "inode ${file2_inode}: type=? path="

	cd ..
}

assert_notifier_log_plain() {
	local log="${1}"

	local expected=(
		"ACCESS(2)"
		"inode 2: type=d path=/folder1"
		"ACCESS(3)"
		"inode 3: type=d path=/folder2"
		"ACCESS(4)"
		"inode 4: type=d path=/folder1/subfolder1"
		"ACCESS(5)"
		"inode 5: type=d path=/folder1/subfolder2"
		"UNLINK(2,file1):6"
		"inode 6: type=t path=/folder1/file1 (trash)"
		"UNLINK(2,file2):7"
		"inode 7: type=r path=/folder1/file2 (reserved)"
		"PURGE(6)"
		"RELEASE(6,1)"
		"inode 6: type=? path="
	)

	for e in "${expected[@]}"; do
		assert_success grep -q "${e}" "${log}"
	done
}

assert_notifier_log_tls() {
	local log="${1}"

	local expected=(
		"ACCESS(8)"
		"inode 8: type=d path=/folder1"
		"ACCESS(9)"
		"inode 9: type=d path=/folder2"
		"ACCESS(10)"
		"inode 10: type=d path=/folder1/subfolder1"
		"ACCESS(11)"
		"inode 11: type=d path=/folder1/subfolder2"
		"UNLINK(8,file1):12"
		"inode 12: type=t path=/folder1/file1 (trash)"
		"UNLINK(8,file2):13"
		"inode 13: type=r path=/folder1/file2 (reserved)"
		"PURGE(12)"
		"RELEASE(12,1)"
		"inode 12: type=? path="
	)

	for e in "${expected[@]}"; do
		assert_success grep -q "${e}" "${log}"
	done
}

cleanup_mountpoint() {
	local mp="${info[mount0]}"

	rm -rf \
		"${mp}/folder1" \
		"${mp}/folder2"
}

run_notifier_test() {
	local mode="$1"        # "tls" or "plain"
	local tls_cfg="$2"     # empty string means no TLS
	local log="${TEMP_DIR}/notifier-${mode}.log"

	echo "=== Running metadata-notifier test (${mode}) ==="
	assert_eventually '[[ "$(count_notifiers)" == 0 ]]'

	if [[ -n "${tls_cfg}" ]]; then
		metadata-notifier localhost "${info[matont]}" "${tls_cfg}" \
			>"${log}" 2>&1 &
	else
		metadata-notifier localhost "${info[matont]}" \
			>"${log}" 2>&1 &
	fi

	local NOTIFIER_PID=$!
	echo "Notifier started with PID ${NOTIFIER_PID}"

	assert_eventually '[[ "$(count_notifiers registered)" == 1 ]]'

	run_metadata_operations "${log}"

	kill -s SIGKILL "${NOTIFIER_PID}"
	wait "${NOTIFIER_PID}" 2>/dev/null || true

	cat "${log}"

	if [[ "${mode}" == "tls" ]]; then
		assert_notifier_log_tls "${log}"
	else
		assert_notifier_log_plain "${log}"
	fi

	cleanup_mountpoint
}

CHUNKSERVERS=1 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfsreportreservedperiod=1" \
	MASTER_EXTRA_CONFIG="${master_cfg}" \
	setup_local_empty_saunafs info

# Without TLS
run_notifier_test "plain" ""

# With TLS
run_notifier_test "tls" "${TEMP_DIR}/sfstls.cfg"

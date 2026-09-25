#!/usr/bin/env bash

readonly workspace="/tmp/saunafs-fdb-test"
# Every write path in the tests commits to this cluster, so its data lives on a tmpfs of its
# own: commit latency must not depend on the node's disk, and the test ramdisk stays free for
# the tests that fill it. FoundationDB throttles writes when its data volume has less than
# 1 GB free, so the tmpfs is sized well above the largest test's data (about 650 MB).
readonly fdb_data_dir="${workspace}/data"
readonly fdb_data_tmpfs_size="3g"
readonly fdbmonitor_pattern="fdbmonitor --conffile ${workspace}/conf/foundationdb\.conf"

function wait_until_processes_stop() {
	local pattern="${1}"
	local timeout_seconds="${2:-5}"
	local deadline=$((SECONDS + timeout_seconds))
	while pgrep -f "${pattern}" > /dev/null 2>&1; do
		if (( SECONDS >= deadline )); then
			return 1
		fi
		sleep 0.1
	done
	return 0
}

function create_workspace() {
	mkdir -p "${workspace}"/{conf,data,logs}
	chown -R "$(id -un):$(id -gn)" "${workspace}"
	mount_fdb_data_tmpfs
}

function mount_fdb_data_tmpfs() {
	if mountpoint -q "${fdb_data_dir}"; then
		return 0
	fi
	if ! sudo -n /usr/bin/mount -t tmpfs -o "size=${fdb_data_tmpfs_size},mode=1777" tmpfs \
			"${fdb_data_dir}" 2>/dev/null; then
		echo "foundationdb.sh: no tmpfs for ${fdb_data_dir}, the cluster data stays on disk" >&2
	fi
}

function unmount_fdb_data_tmpfs() {
	if mountpoint -q "${fdb_data_dir}"; then
		sudo -n /usr/bin/umount -l "${fdb_data_dir}" 2>/dev/null || true
	fi
}

function create_config_file() {
	cat > "${workspace}/conf/foundationdb.conf" <<EOF
[fdbmonitor]
user = $(id -un)
group = $(id -gn)
lockfile = ${workspace}/fdbmonitor.pid

[general]
restart-delay = 60
cluster-file = ${workspace}/conf/fdb.cluster

[fdbserver]
command = /usr/sbin/fdbserver
public_address = 127.0.0.1:\$ID
listen_address = public
datadir = ${fdb_data_dir}/\$ID
logdir = ${workspace}/logs

[fdbserver.4500]
[fdbserver.4501]

[backup_agent]
command = /usr/lib/foundationdb/backup_agent/backup_agent
logdir = ${workspace}/logs

[backup_agent.1]
EOF
}

function check_cluster_status() {
	fdbcli --exec "status" --cluster-file "${workspace}/conf/fdb.cluster"
}

function cleanup_fdb_cluster() {
	# Kill fdbmonitor (runs as root); escalate to SIGKILL if SIGTERM does not
	# stop it within the grace period.
	sudo -n pkill -f "${fdbmonitor_pattern}" 2>/dev/null || true
	if ! wait_until_processes_stop "${fdbmonitor_pattern}" 5; then
		sudo -n pkill -9 -f "${fdbmonitor_pattern}" 2>/dev/null || true
		wait_until_processes_stop "${fdbmonitor_pattern}" 2 || true
	fi

	# Ensure worker processes bound to the test workspace are not left alive;
	# escalate to SIGKILL for each worker pattern on timeout.
	local worker_patterns=("fdbserver.*${fdb_data_dir}/" "backup_agent.*${workspace}/logs")
	for pattern in "${worker_patterns[@]}"; do
		pkill -f "${pattern}" 2>/dev/null || true
	done
	for pattern in "${worker_patterns[@]}"; do
		if ! wait_until_processes_stop "${pattern}" 2; then
			pkill -9 -f "${pattern}" 2>/dev/null || true
			wait_until_processes_stop "${pattern}" 1 || true
		fi
	done

	unmount_fdb_data_tmpfs
	rm -rf "${workspace:?}" 2>/dev/null || true
}

function start_fdb_cluster() {
	cleanup_fdb_cluster
	create_workspace
	fdb_cluster_started=1

	# Create cluster file manually
	if [ ! -f "${workspace}/conf/fdb.cluster" ]; then
		echo "Creating new cluster configuration..."
		local -r description="saunafstest"
		# Declare then assign so the command's exit status is not masked (ShellCheck SC2155).
		local cluster_id
		cluster_id="$(mktemp -u XXXXXXXX)"
		echo "${description}:${cluster_id}@127.0.0.1:4500" > "${workspace}/conf/fdb.cluster"
		chmod 644 "${workspace}/conf/fdb.cluster"
	fi

	create_config_file

	# Start FoundationDB with required sudo privileges. Detach via a subshell so the
	# test shell's bare `wait` does not block on this long-running daemon.
	( sudo /usr/lib/foundationdb/fdbmonitor --conffile "${workspace}/conf/foundationdb.conf" & )

	# Ensure cluster is configured
	fdbcli --exec "configure new single memory" --cluster-file "${workspace}/conf/fdb.cluster"

	# Wait for fdbmonitor to initialize
	sleep 3
}

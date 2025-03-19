#!/usr/bin/env bash

# Starts a temporary FoundationDB Cluster in /tmp/saunafs-fdb-test.
# The cluster uses two servers on ports 4500 and 4501.
# This script is intended for testing purposes only.

set -eu -o pipefail

readonly script_name=$(basename "$0")
readonly workspace="/tmp/saunafs-fdb-test"

readonly action="${1:-}"

function create_workspace() {
	mkdir -p "${workspace}"/{conf,data,logs}
	chown -R "$(id -un):$(id -gn)" "${workspace}"
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
datadir = ${workspace}/data/\$ID
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
	sudo pkill -f fdbmonitor || true
	rm -r "${workspace:?}" || true
}

function start_fdb_cluster() {
	cleanup_fdb_cluster
	create_workspace

	# Create cluster file manually
	if [ ! -f "${workspace}/conf/fdb.cluster" ]; then
		echo "Creating new cluster configuration..."
		readonly description="saunafstest"
		readonly cluster_id="$(mktemp -u XXXXXXXX)"
		echo "${description}:${cluster_id}@127.0.0.1:4500" > "${workspace}/conf/fdb.cluster"
		chmod 644 "${workspace}/conf/fdb.cluster"
	fi

	create_config_file

	# Start FoundationDB with required sudo privileges
	sudo /usr/lib/foundationdb/fdbmonitor --conffile "${workspace}/conf/foundationdb.conf" &

	# Wait for fdbmonitor to initialize
	sleep 3

	# Ensure cluster is configured
	fdbcli --exec "configure new single memory" --cluster-file "${workspace}/conf/fdb.cluster"

	check_cluster_status
}

function stop_fdb_cluster() {
	cleanup_fdb_cluster
}

function main() {
  case "${action}" in
    start)
      start_fdb_cluster
      ;;
    stop)
      stop_fdb_cluster
      ;;
    *)
      echo "Usage: ${script_name} {start|stop}"
      exit 1
      ;;
  esac
}

main "$@"

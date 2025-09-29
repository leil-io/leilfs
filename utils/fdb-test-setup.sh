#!/usr/bin/env bash
set -e

: "${FDB_SERVER_PORT:=4560}"

is_port_open() {
    ss -Hnl "sport = :${1}" | grep -q .
}
export -f is_port_open

wait_for_port() {
	local port=${1}
	local timeout=${2:-10}
    timeout ${timeout} bash -c 'while ! is_port_open '"${port}"'; do sleep 1; done'
}

wait_for_init(){
	timeout "${1:-120}" bash -s << 'EOF'
output="$(fdbcli -C ~/fdb-test/fdb.cluster --exec "status")"
while grep -qE "initializing|unknown" <<< "${output}"; do
	sleep 1
	output="$(fdbcli -C ~/fdb-test/fdb.cluster --exec "status")"
done
EOF
}

# Create a test cluster file
mkdir -p ~/fdb-test/{data,logs}
cat > ~/fdb-test/fdb.cluster << EOF
test:test@127.0.0.1:${FDB_SERVER_PORT}
EOF

# Start a simple single-node cluster
fdbserver -p "127.0.0.1:${FDB_SERVER_PORT}" \
  -C ~/fdb-test/fdb.cluster \
  -d ~/fdb-test/data \
  -L ~/fdb-test/logs &

# Wait for it to start
wait_for_port "${FDB_SERVER_PORT}" 10

# Initialize it
fdbcli -C ~/fdb-test/fdb.cluster --exec "configure new single memory" &>/dev/null || true

# Allow it to initialize
wait_for_init 120

# Test it
fdbcli -C ~/fdb-test/fdb.cluster --exec "status"

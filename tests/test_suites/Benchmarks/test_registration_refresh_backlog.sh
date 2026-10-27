timeout_set '2 hours'

# Failover with an EC(3,2) hot set and a controlled disk-state refresh backlog.
# Mock background inventory is goal 1, NOT a full production EC memory model.
# See Benchmarks/registration_refresh_backlog.md for parameters and limitations.
CHUNK_COUNT=${CHUNK_COUNT:-100000}
CS_COUNT=${CS_COUNT:-5}
TARGET_COUNT=${TARGET_COUNT:-512}
WRITERS=${WRITERS:-8}
REGISTRATION_BUDGET=${REGISTRATION_BUDGET:-50000}
HOLD_SECONDS=${HOLD_SECONDS:-3}
REFRESH_ACTION=${REFRESH_ACTION:-drain}
MAX_MASTER_REPLY_MS=${MAX_MASTER_REPLY_MS:-0}
REQUIRE_INVENTORY_OVERLAP=${REQUIRE_INVENTORY_OVERLAP:-0}

assert_success test "$CS_COUNT" -ge 5
assert_success test "$TARGET_COUNT" -ge "$WRITERS"
assert_success test "$CHUNK_COUNT" -ge 1000
assert_success test "$REGISTRATION_BUDGET" -gt 0
assert_success test "$REFRESH_ACTION" = drain -o "$REFRESH_ACTION" = disconnect

master_config="CHUNK_REGISTRATION_CHUNKS_PER_SECOND = $REGISTRATION_BUDGET"
master_config+='|CHUNK_REGISTRATION_BULK_SIZE = 100'
master_config+='|ON_DEMAND_CHUNK_QUERY_TIMEOUT_MS = 30000'

# Isolate refresh scheduling from background repair/evacuation in this benchmark.
master_config+='|OPERATIONS_DELAY_INIT = 3600|OPERATIONS_DELAY_DISCONNECT = 3600'

# Pin name storage in both generated roles: promotion reloads cannot switch it.
# The harness otherwise chooses independently for master and shadow configs.
create_bdb_name_storage_entry_() {
	echo 'USE_BDB_FOR_NAME_STORAGE = 0'
}

MASTERSERVERS=2 \
	CHUNKSERVERS=$CS_COUNT \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
	MASTER_CUSTOM_GOALS='10 ec32: $ec(3,2)' \
	MASTER_EXTRA_CONFIG="$master_config" \
	CHUNKSERVER_EXTRA_CONFIG='MASTER_RECONNECTION_DELAY = 1|MASTER_TIMEOUT = 300' \
	MOUNT_EXTRA_CONFIG='sfscachemode=NEVER' \
	setup_local_empty_saunafs info

workload="${SOURCE_DIR}/tests/tools/registration_stress_workload.py"
metrics="$TEMP_DIR/refresh-metrics"
proxy="$metrics/proxy"
mkdir -p "$proxy"

pids=()

cleanup() {
	local status=$? pid

	for pid in "${pids[@]}"; do
		kill "$pid" 2>/dev/null || true
	done

	for pid in "${pids[@]}"; do
		wait "$pid" 2>/dev/null || true
	done

	if ((status != 0)); then
		for file in "$metrics"/*.log "$proxy/state.json"; do
			[[ ! -f "$file" ]] || tail -30 "$file"
		done
	fi
}

trap cleanup EXIT

now_ms() {
	echo "$(($(date +%s%N) / 1000000))"
}

phase() {
	printf '%s\n' "$1" > "$metrics/phase.new"
	mv "$metrics/phase.new" "$metrics/phase"
	echo "PHASE: $1"
}

metric() {
	python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])' "$1" "$2"
}

# Seed disjoint standard-chunk ranges, as in test_failover_under_load. This
# increases registry lookup pressure without creating millions of real files.
background=$((CS_COUNT * CHUNK_COUNT))
saunafs_master_daemon stop
metadata_version=$(metadata_get_version "${info[master_data_path]}/metadata.sfs")
generate_mock_chunks_changelog "$background" 1000 "$metadata_version" \
	> "${info[master_data_path]}/changelog.sfs"
assert_success sfsmetarestore -a -d "${info[master_data_path]}"

saunafs_master_daemon start
saunafs_wait_for_all_ready_chunkservers

for ((cs = 0; cs < CS_COUNT; ++cs)); do
	mock_dir="$TEMP_DIR/mock_disk_$cs"
	mkdir "$mock_dir"

	echo "mock:$CHUNK_COUNT:$((cs * CHUNK_COUNT + 1)):$mock_dir" \
		>> "${info[chunkserver${cs}_hdd]}"
	saunafs_chunkserver_daemon "$cs" reload
done

assert_eventually_equals "echo $background" 'count_chunk_copies' '30 minutes'

# Untouched target files get their first post-promotion write during the race.
# A separate hot set sustains I/O across failover without consuming that bump.
targets="${info[mount0]}/refresh-targets"
hot="${info[mount0]}/refresh-load"
mkdir "$targets" "$hot"
assert_success saunafs setgoal ec32 "$targets" "$hot"

assert_success python3 "$workload" seed "$targets" --count "$TARGET_COUNT" \
	--workers "$WRITERS" --output "$metrics/seed.json"
assert_success python3 "$workload" seed "$hot" --count 16 \
	--output "$metrics/hot-seed.json"

manifest="$metrics/manifest.json"
assert_success python3 "$workload" manifest "$targets" \
	--source-disk "$RAMDISK_DIR/hdd_0_0" --output "$manifest"

expected=$(metric "$manifest" source_parts)
assert_success test "$expected" -gt 0

# Synchronize the future master before starting failover measurements.
saunafs_master_n 1 start
assert_eventually 'saunafs_shadow_synchronized 1' '10 minutes'

master_pid=$(saunafs_master_n 1 test | awk '/pid:/ {print $NF}')
assert_success test -n "$master_pid"

phase warmup

python3 "$workload" probe --host "$(get_ip_addr)" --port "${info[matocl]}" \
	--pid "$master_pid" --phase "$metrics/phase" --stop "$metrics/probe-stop" \
	--output "$metrics/probe.json" > "$metrics/probe.log" 2>&1 &
probe_pid=$!
pids+=("$probe_pid")

python3 "$workload" write "$hot" --count 16 --stop "$metrics/load-stop" \
	--phase "$metrics/phase" --output "$metrics/load.json" > "$metrics/load.log" 2>&1 &
load_pid=$!
pids+=("$load_pid")

sleep 2

# CS0 must finish its inventory before its disk reload can emit CHUNK_NEW.
# Gate CS1's credits to keep the cluster's inventory incomplete throughout.
saunafs_chunkserver_daemon 0 stop
saunafs_chunkserver_daemon 1 stop

phase promotion
promotion_start=$(now_ms)
saunafs_master_daemon kill
saunafs_make_conf_for_shadow 0
saunafs_make_conf_for_master 1
saunafs_master_daemon reload

get_next_port_number proxy_port
get_next_port_number gate_port

python3 "${SOURCE_DIR}/tests/tools/registration_stress_proxy.py" \
	--host "$(get_ip_addr)" --listen-port "$proxy_port" --master-port "${info[matocs]}" \
	--manifest "$manifest" --directory "$proxy" > "$metrics/proxy.log" 2>&1 &
proxy_pid=$!
pids+=("$proxy_pid")

python3 "${SOURCE_DIR}/tests/tools/registration_credit_gate.py" \
	--host "$(get_ip_addr)" --listen-port "$gate_port" --master-port "${info[matocs]}" \
	--ready-marker "$metrics/gate-ready" --held-marker "$metrics/gate-held" \
	--release-marker "$metrics/gate-release" --end-marker "$metrics/gate-end" \
	> "$metrics/gate.log" 2>&1 &
gate_pid=$!
pids+=("$gate_pid")

assert_eventually 'test -f "$proxy/ready" -a -f "$metrics/gate-ready"'

# Reconnect CS0 through the proxy so its stale disk reports and refresh replies
# can be held to build the backlog. The proxy forwards to the promoted master.
add_lines_sfschunkserver_cfg_ "MASTER_HOST = $(get_ip_addr)|MASTER_PORT = $proxy_port" 0

# Reconnect CS1 through the credit gate, which forwards to the same master but
# holds further registration credits until the benchmark releases them.
add_lines_sfschunkserver_cfg_ "MASTER_HOST = $(get_ip_addr)|MASTER_PORT = $gate_port" 1

saunafs_chunkserver_daemon 0 start
saunafs_chunkserver_daemon 1 start
assert_eventually 'test -f "$proxy/registered" -a -f "$metrics/gate-held"' '10 minutes'

for ((cs = 2; cs < CS_COUNT; ++cs)); do
	wait_for_chunkserver_pull_registration "$cs"
done

assert_failure test -f "$metrics/gate-end"
echo "PROMOTION_SOURCE_REGISTRATION_MS: $(($(now_ms) - promotion_start))"

# Capture genuine old flags, then change each existing target's version once.
# Replies are held only after the source's complete registration, avoiding
# confusion between client-location queries and the intended refresh backlog.
phase conflicts
touch "$proxy/arm"
sed -i '1s/^/*/' "${info[chunkserver0_hdd]}"
saunafs_chunkserver_daemon 0 reload
assert_eventually_equals "echo $expected" 'metric "$proxy/state.json" held_reports'

assert_success python3 "$workload" write "$targets" --count "$TARGET_COUNT" \
	--workers "$WRITERS" --output "$metrics/writes.json"
assert_eventually_equals "echo $expected" 'metric "$proxy/state.json" released_reports'

# Fill the scheduler's 128-key per-peer window before measuring either action.
outstanding=$((expected < 128 ? expected : 128))
assert_eventually_equals "echo $outstanding" 'metric "$proxy/state.json" held_query_ids'
assert_failure test -f "$metrics/gate-end"
assert_success kill -0 "$proxy_pid" "$gate_pid" "$load_pid" "$probe_pid"

# Resume paced bulks while query replies remain held. With larger inventories,
# the backlog actions below overlap actual inventory traffic, not just a flag.
phase backlog
touch "$metrics/gate-release"
sleep "$HOLD_SECONDS"

# Drain tests eventual fresh-state delivery; disconnect measures responsiveness
# while the peer's pending map/queues and existing copy cleanup are destroyed.
phase "$REFRESH_ACTION"

if [[ -f "$metrics/gate-end" ]]; then
	echo 'INVENTORY_STREAMING_AT_ACTION: 0'
else
	echo 'INVENTORY_STREAMING_AT_ACTION: 1'
fi

if [[ "$REQUIRE_INVENTORY_OVERLAP" == 1 ]]; then
	assert_failure test -f "$metrics/gate-end"
fi

action_start=$(now_ms)
if [[ "$REFRESH_ACTION" == drain ]]; then
	touch "$proxy/release"
	assert_eventually_equals "echo $expected" 'metric "$proxy/state.json" fresh_parts'
else
	touch "$proxy/disconnect"

	wait "$proxy_pid"
	assert_equals 0 "$?"
	saunafs_chunkserver_daemon 0 stop
fi

echo "REFRESH_${REFRESH_ACTION^^}_WIRE_MS: $(($(now_ms) - action_start))"
sleep "$HOLD_SECONDS"

phase post
assert_eventually 'test -f "$metrics/gate-end"' '10 minutes'
echo "PROMOTION_GATE_COMPLETION_MS: $(($(now_ms) - promotion_start))"

assert_success python3 "$workload" verify "$manifest"

# Keep the probe running until all in-flight workload operations finish.
touch "$metrics/load-stop"
wait "$load_pid"
assert_equals 0 "$?"

touch "$metrics/probe-stop"
wait "$probe_pid"
assert_equals 0 "$?"

assert_success kill -0 "$gate_pid"
if [[ "$REFRESH_ACTION" == drain ]]; then
	assert_success kill -0 "$proxy_pid"
fi

echo "INVENTORY: standard_background=$background ec32_targets=$TARGET_COUNT" \
	"source_parts=$expected cs=$CS_COUNT"
assert_success python3 "$workload" report --directory "$metrics" \
	--max-master-reply-ms "$MAX_MASTER_REPLY_MS"

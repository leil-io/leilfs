timeout_set '4 hours'

# Production-style failover simulation. Not pass/fail -- prints numbers
# (see Benchmarks/README.md).
#
# Models a cluster whose chunkservers host CHUNK_COUNT mock chunks each
# (memory-backed, no chunk files) while several clients keep reading cold
# mock-backed files and writing/reading-back real files. Mid-load the master
# is killed and a shadow is promoted; the interesting output is how long each
# client stream stalls around the failover and how long the full paced
# re-registration takes.
#
# Parameters (env):
#   CHUNK_COUNT     mock chunks per chunkserver (default 10'000'000).
#                   RAM: ~180B/chunk per chunkserver + ~170B/copy on the
#                   master (~350B per copy in total). Guidance:
#                     32GB box:  15M/cs x 4 cs (60M copies, ~21GB)
#                     64GB box:  25M/cs x 4 cs (100M copies, ~35GB, ~11min seeding)
#                     150GB+:    100M/cs x 4 cs (per-cs production scale,
#                                ~1h seeding -- cache the seeded
#                                metadata.sfs when iterating)
#   MOCK_CS_COUNT   number of chunkservers (default 4)
#   READER_COUNT    reader clients (default 3); writers = MOUNTS - readers
#   REGISTRATION_BUDGET  CHUNK_REGISTRATION_CHUNKS_PER_SECOND (default 500000,
#                   matching the shipped paced default; set 0 for an
#                   unpaced baseline)

CHUNK_COUNT=${CHUNK_COUNT:-10000000}
MOCK_CS_COUNT=${MOCK_CS_COUNT:-4}
READER_COUNT=${READER_COUNT:-3}
MOUNT_COUNT=5
CHUNKS_PER_FILE=10000
LOAD_WARMUP_SECONDS=5

REGISTRATION_BUDGET=${REGISTRATION_BUDGET:-500000}

echo "REGISTRATION_BUDGET: ${REGISTRATION_BUDGET}"
MASTERSERVERS=2 \
	CHUNKSERVERS=$MOCK_CS_COUNT \
	MOUNTS=$MOUNT_COUNT \
	AUTO_SHADOW_MASTER=NO \
	MASTER_EXTRA_CONFIG="CHUNK_REGISTRATION_CHUNKS_PER_SECOND = ${REGISTRATION_BUDGET}" \
	CHUNKSERVER_EXTRA_CONFIG="MASTER_RECONNECTION_DELAY = 1|HDD_TEST_FREQ = 100000" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

now_ms() { echo $(($(date +%s%N) / 1000000)); }

total_copies=$((MOCK_CS_COUNT * CHUNK_COUNT))
total_files=$((total_copies / CHUNKS_PER_FILE))

# ---- Seed metadata offline (disjoint id ranges per chunkserver, goal 1) ----
seed_start_ms=$(now_ms)
saunafs_master_daemon stop
metadata_version=$(metadata_get_version "${info[master_data_path]}/metadata.sfs")
generate_mock_chunks_changelog "$total_copies" "$CHUNKS_PER_FILE" "$metadata_version" \
	> "${info[master_data_path]}/changelog.sfs"
assert_success sfsmetarestore -a -d "${info[master_data_path]}"
saunafs_master_daemon start
saunafs_wait_for_all_ready_chunkservers
echo "METADATA_SEEDING_MS: $(($(now_ms) - seed_start_ms))"

# ---- Attach the mock disks ----
initial_start_ms=$(now_ms)
for ((csid = 0; csid < MOCK_CS_COUNT; ++csid)); do
	mock_dir="$TEMP_DIR/mock_disk_$csid"
	mkdir -p "$mock_dir"
	echo "mock:${CHUNK_COUNT}:$((csid * CHUNK_COUNT + 1)):${mock_dir}" \
		>> "${info[chunkserver${csid}_hdd]}"
	saunafs_chunkserver_daemon "$csid" reload
done
while [[ "$(count_chunk_copies)" != "$total_copies" ]]; do
	sleep 1
done
echo "INITIAL_REGISTRATION_MS: $(($(now_ms) - initial_start_ms))"

# ---- Start and sync the shadow ----
saunafs_master_n 1 start
assert_eventually "saunafs_shadow_synchronized 1" '30 minutes'

# ---- Client load ----
# Readers pick a random (usually location-cold) mock file each iteration;
# writers create+fsync+read-back small files in their own directory. Each
# stream records aggregate metrics for the whole run and for operations that
# overlap the promotion window, so a promotion stall is attributable without
# writing an unbounded per-operation trace.
stop_file="$TEMP_DIR/load_stop"
metrics_dir="$TEMP_DIR/load_metrics"
mkdir -p "$metrics_dir"
load_phase_file="$metrics_dir/phase"
set_load_phase() {
    printf '%s\n' "$1" > "${load_phase_file}.new"
    mv "${load_phase_file}.new" "$load_phase_file"
}
set_load_phase warmup

reader_loop() {
	local mount_dir=$1
	local out=$2
	local ops=0 errors=0 worst=0
	local promotion_ops=0 promotion_errors=0 promotion_worst=0
	while [[ ! -f "$stop_file" ]]; do
		local idx=$(( (RANDOM * 32768 + RANDOM) % total_files ))
		local file
		file="$mount_dir/$(printf 'mock_%07d' "$idx")"
		local phase_before phase_after status=0 t0
		phase_before=$(<"$load_phase_file")
		t0=$(date +%s%N)
		dd if="$file" bs=16 count=1 > /dev/null 2>&1 || {
			errors=$((errors + 1))
			status=1
		}
		local dt=$(( ($(date +%s%N) - t0) / 1000000 ))
		phase_after=$(<"$load_phase_file")
		(( dt > worst )) && worst=$dt
		if [[ "$phase_before" == promotion || "$phase_after" == promotion ]]; then
			(( dt > promotion_worst )) && promotion_worst=$dt
			promotion_ops=$((promotion_ops + 1))
			(( status != 0 )) && promotion_errors=$((promotion_errors + 1))
		fi
		ops=$((ops + 1))
	done
	echo "$ops $worst $errors $promotion_ops $promotion_worst $promotion_errors" > "$out"
}

writer_loop() {
	local mount_dir=$1
	local out=$2
	local stream_id=$3
	local ops=0 errors=0 worst=0
	local promotion_ops=0 promotion_errors=0 promotion_worst=0
	mkdir -p "$mount_dir/writer_dir_$stream_id" 2>/dev/null
	while [[ ! -f "$stop_file" ]]; do
		local file="$mount_dir/writer_dir_$stream_id/f_$((ops % 50))"
		local phase_before phase_after status=0 t0
		phase_before=$(<"$load_phase_file")
		t0=$(date +%s%N)
		if printf 'payload %s\n' "$ops" | dd of="$file" conv=fsync status=none 2>/dev/null &&
				[[ "$(cat "$file" 2>/dev/null)" == "payload $ops" ]]; then
			:
		else
			errors=$((errors + 1))
			status=1
		fi
		local dt=$(( ($(date +%s%N) - t0) / 1000000 ))
		phase_after=$(<"$load_phase_file")
		(( dt > worst )) && worst=$dt
		if [[ "$phase_before" == promotion || "$phase_after" == promotion ]]; then
			(( dt > promotion_worst )) && promotion_worst=$dt
			promotion_ops=$((promotion_ops + 1))
			(( status != 0 )) && promotion_errors=$((promotion_errors + 1))
		fi
		ops=$((ops + 1))
	done
	echo "$ops $worst $errors $promotion_ops $promotion_worst $promotion_errors" > "$out"
}

for ((m = 0; m < MOUNT_COUNT; ++m)); do
	if ((m < READER_COUNT)); then
		reader_loop "${info[mount$m]}" "$metrics_dir/stream_$m" &
		echo "client $m: reader"
	else
		writer_loop "${info[mount$m]}" "$metrics_dir/stream_$m" "$m" &
		echo "client $m: writer"
	fi
done
sleep "$LOAD_WARMUP_SECONDS"

# ---- Failover under load ----
promotion_start_ms=$(now_ms)
set_load_phase promotion
saunafs_master_daemon kill
saunafs_make_conf_for_shadow 0
saunafs_make_conf_for_master 1
saunafs_master_daemon reload
echo "PROMOTION at $(date +%T)"

# Full re-registration on the promoted master (>=: the writer clients keep
# creating new chunks during the failover, so the count overshoots)
while true; do
	copies=$(count_chunk_copies)
	[[ -n "$copies" && "$copies" -ge "$total_copies" ]] && break
	sleep 1
done
echo "PROMOTION_FULL_REGISTRATION_MS: $(($(now_ms) - promotion_start_ms))"
set_load_phase post

# Keep the load running a moment after convergence, then stop it
sleep 5
touch "$stop_file"
wait

# ---- Report ----
for ((m = 0; m < MOUNT_COUNT; ++m)); do
	read -r ops worst errors promotion_ops promotion_worst promotion_errors < "$metrics_dir/stream_$m"
	kind=reader; ((m >= READER_COUNT)) && kind=writer
	echo "CLIENT_${m}_${kind}: ops=$ops max_latency_ms=$worst errors=$errors" \
		"promotion_ops=$promotion_ops promotion_max_latency_ms=$promotion_worst" \
		"promotion_errors=$promotion_errors"
done

# several processes can match (symlink names, transient reload commands);
# the actual master is the one with the largest RSS
master_pid=$(pgrep -f "sfsmaster|leil-master" | xargs -r ps -o pid=,rss= -p 2>/dev/null \
	| sort -k2 -rn | head -1 | awk '{print $1}')
[[ $master_pid ]] && echo "MASTER_RSS_KB: $(awk '/VmRSS/ {print $2}' /proc/$master_pid/status)"
cs_pid=$(pgrep -f "(sfschunkserver|leil-chunkserver).*${info[chunkserver0_cfg]}" | head -1)
[[ $cs_pid ]] && echo "CHUNKSERVER_RSS_KB: $(awk '/VmRSS/ {print $2}' /proc/$cs_pid/status)"

echo "CHUNK_COUNT: $CHUNK_COUNT/cs, MOCK_CS_COUNT: $MOCK_CS_COUNT," \
	"READERS: $READER_COUNT, WRITERS: $((MOUNT_COUNT - READER_COUNT))," \
	"REGISTRATION_BUDGET: $REGISTRATION_BUDGET/s"

# Sanity: the data survived the failover
expected_head="5a5b58595e5f5c5d5253505156575455"
assert_equals "$expected_head" \
	"$(dd if="${info[mount0]}/mock_0000000" bs=16 count=1 2>/dev/null | od -An -tx1 | tr -d ' \n')"

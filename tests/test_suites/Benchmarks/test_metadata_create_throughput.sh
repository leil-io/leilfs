timeout_set 30 minutes

# Baseline metadata create throughput (files/sec) for the FDB forkless backend.
#
# Purpose: a stable reference point to compare the current SYNCHRONOUS FDB commit against a future
# ASYNCHRONOUS commit. Empty files are used so the cost is the metadata path (node + edge create ->
# FDB commit), not the chunkserver data path.
#
# It reports three things, because synchronous commit hurts in two different ways:
#   - sequential throughput  : raw files/sec, one client (median of 3 runs).
#   - concurrent throughput  : files/sec with K parallel creators -- this is where a synchronous
#                              commit that blocks the event loop shows its ceiling.
#   - per-create latency     : median / p99 / max for a small probe -- exposes event-loop stalls
#                              that a throughput average hides.
#
# This is a benchmark, not a pass/fail test: it prints numbers (and only fails if creates are
# silently dropped). Run the same script before and after the async-commit change, same host, and
# compare. Reference ceiling: run with METADATA_BACKEND unset (FILE backend, no FDB commit).
#
# Tunables (env): THROUGHPUT_FILES (default 20000), THROUGHPUT_WORKERS (8),
#                 THROUGHPUT_PROBE (2000), THROUGHPUT_WARMUP (5000).

N=${THROUGHPUT_FILES:-20000}
K=${THROUGHPUT_WORKERS:-8}
PROBE_N=${THROUGHPUT_PROBE:-2000}
WARMUP_N=${THROUGHPUT_WARMUP:-5000}
bench_backend="${METADATA_BACKEND:-FORKLESS}"

CHUNKSERVERS=1 \
	MASTERSERVERS=1 \
	USE_RAMDISK=YES \
	METADATA_BACKEND="$bench_backend" \
	AUTO_SHADOW_MASTER="NO" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER,sfsdirentrycacheto=0" \
	MASTER_EXTRA_CONFIG="METADATA_DUMP_PERIOD_SECONDS = 0" \
	setup_local_empty_saunafs info

# files/sec from (count, start, end); guards against a zero/negative interval.
rate() { awk -v n="$1" -v s="$2" -v e="$3" 'BEGIN { d = e - s; if (d <= 0) { d = 0.000001 } printf "%.0f", n / d }'; }
# median of the numeric args.
median() { printf '%s\n' "$@" | sort -n | awk '{ a[NR] = $0 } END { print a[int((NR + 1) / 2)] }'; }

cd "${info[mount0]}"

# --- warm-up: spin up the writer/flush machinery and steady-state the create path (discarded). ---
mkdir -p warmup
for i in $(seq 1 "$WARMUP_N"); do > "warmup/w_$i"; done
rm -rf warmup

# --- sequential: median of 3 runs, fresh subdir each run (create cost should not include the rm). ---
seq_rates=()
for run in 1 2 3; do
	mkdir -p "seq_$run"
	start=$EPOCHREALTIME
	for i in $(seq 1 "$N"); do > "seq_$run/f_$i"; done
	end=$EPOCHREALTIME
	r=$(rate "$N" "$start" "$end")
	seq_rates+=("$r")
	echo "sequential run $run: $N files in $(awk -v s="$start" -v e="$end" 'BEGIN { printf "%.2f", e - s }')s -> $r files/s"
	# Sanity: every create must have landed (catches silently dropped ops).
	created=$(ls -1 "seq_$run" | wc -l)
	assert_equals "$N" "$created"
	rm -rf "seq_$run"
done
seq_median=$(median "${seq_rates[@]}")

# --- concurrent: K parallel creators, separate subdirs. The wall clock spans all workers. ---
mkdir -p conc
per=$((N / K))
start=$EPOCHREALTIME
for k in $(seq 1 "$K"); do
	(
		mkdir -p "conc/$k"
		for i in $(seq 1 "$per"); do > "conc/$k/f_$i"; done
	) &
done
wait
end=$EPOCHREALTIME
conc_total=$((per * K))
conc_rate=$(rate "$conc_total" "$start" "$end")
echo "concurrent x$K: $conc_total files in $(awk -v s="$start" -v e="$end" 'BEGIN { printf "%.2f", e - s }')s -> $conc_rate files/s"
rm -rf conc

# --- per-create latency probe: time each create individually (EPOCHREALTIME is fork-free), then
#     compute the distribution in one awk pass. Exposes event-loop stalls a mean would hide. ---
mkdir -p probe
stamps=()
for i in $(seq 1 "$PROBE_N"); do
	t0=$EPOCHREALTIME
	> "probe/p_$i"
	t1=$EPOCHREALTIME
	stamps+=("$t0 $t1")
done
printf '%s\n' "${stamps[@]}" \
	| awk '{ printf "%.4f\n", ($2 - $1) * 1000 }' \
	| sort -n > "${TEMP_DIR}/lat_ms.txt"
rm -rf probe

lat_median=$(awk '{ a[NR] = $0 } END { print a[int((NR + 1) / 2)] }' "${TEMP_DIR}/lat_ms.txt")
lat_p99=$(awk '{ a[NR] = $0 } END { i = int(NR * 0.99); if (i < 1) { i = 1 } print a[i] }' "${TEMP_DIR}/lat_ms.txt")
lat_max=$(tail -n1 "${TEMP_DIR}/lat_ms.txt")

cd

echo "=== metadata create throughput (backend=${bench_backend}) ==="
echo "sequential  : ${seq_median} files/s (median of ${seq_rates[*]})"
echo "concurrent  : ${conc_rate} files/s (x${K} workers)"
echo "create latency (ms): median=${lat_median}  p99=${lat_p99}  max=${lat_max}  (n=${PROBE_N})"
echo "==========================================================================="

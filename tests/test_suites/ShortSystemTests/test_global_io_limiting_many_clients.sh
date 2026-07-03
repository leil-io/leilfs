timeout_set 1 minute

# Create a config file with a limit of 1 MB/s for all processes
iolimits="$TEMP_DIR/iolimits.cfg"
echo "limit unclassified 1024" > "$iolimits"

# number of mounts
N=5
# Tolerated relative error (in %) for the strict fast side.
E=11
if is_windows_system; then
	E=15
fi
# Slow-side tolerance (in %): loose, reads can stall on busy CI machines.
E_SLOW=$((50 * $(timeout_get_total_multiplier)))
# Fast-side absolute grace (in ns) for the limiter's token bucket bursts.
# Larger here: the sequential reader start-up gives the bucket more time to refill.
FAST_GRACE=$((500 * 1000 * 1000))

CHUNKSERVERS=3 \
	MOUNTS=$N \
	USE_RAMDISK=YES \
	MASTER_EXTRA_CONFIG="GLOBALIOLIMITS_FILENAME = $iolimits" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"
truncate -s1M file

# start one reader on the first mountpoint, two readers on the second mountpoint and so on
start_readers() {
	for ((mount=0; mount < N; mount++)); do
		cd "${info[mount$mount]}"
		for ((reader=0; reader <= mount; reader++)); do
			cat file
		done
	done
}

# consume accumulated limit
cat file >/dev/null

# start measurements
start=$(nanostamp)
i=1
start_readers | while [ $(head -c1M |wc -c) = $((1024 * 1024)) ]; do
	now=$(nanostamp)
	time=$((now - start))
	expected=$((i * 1000 * 1000 * 1000))
	abserr=$((time - expected))
	relerr=$((100 * abserr / expected))
	echo $expected $time $abserr $relerr
	assert_less_or_equal $((expected * (100 - E) / 100 - FAST_GRACE)) ${time}
	assert_less_or_equal ${time} $((expected * (100 + E_SLOW) / 100))
	i=$((i+1))
done

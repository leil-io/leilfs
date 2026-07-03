timeout_set 1 minute

# Create a config file with a limit of 10 MB/s for all processes
iolimits="$TEMP_DIR/iolimits.cfg"
echo "limit unclassified 10240" > "$iolimits"

# number of mounts
N=5
# number of rounds
R=3
# Tolerated relative error (in %) for the strict fast side.
E=11
if is_windows_system; then
	E=15
fi
# Slow-side tolerance (in %): loose, reads can stall on busy CI machines.
E_SLOW=$((50 * $(timeout_get_total_multiplier)))
# Fast-side absolute grace (in ns) for the limiter's token bucket bursts.
FAST_GRACE=$((250 * 1000 * 1000))

CHUNKSERVERS=3 \
	MOUNTS=$N \
	USE_RAMDISK=YES \
	MASTER_EXTRA_CONFIG="GLOBALIOLIMITS_FILENAME = $iolimits" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

truncate -s10M "${info[mount0]}/file"

# consume accumulated limit
cat "${info[mount0]}/file" >/dev/null

total_time=0
total_expected=0
for ((round=0; round < R; round++)); do
	for ((mount=0; mount < N; mount++)); do
		start=$(nanostamp)
		cat "${info[mount$mount]}/file" >/dev/null
		end=$(nanostamp)
		expected=$((1000 * 1000 * 1000))
		time=$((end - start))
		abserr=$((time - expected))
		relerr=$((100 * abserr / expected))
		echo $expected $time $abserr $relerr
		assert_less_or_equal $((expected * (100 - E) / 100 - FAST_GRACE)) ${time}
		total_time=$((total_time + time))
		total_expected=$((total_expected + expected))
	done
done
assert_less_or_equal ${total_time} $((total_expected * (100 + E_SLOW) / 100))

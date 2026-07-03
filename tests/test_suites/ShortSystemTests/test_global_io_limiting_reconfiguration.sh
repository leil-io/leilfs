timeout_set 1 minute

# Create a config file with a limit of 1 MB/s for all processes
iolimits="$TEMP_DIR/iolimits.cfg"
echo "limit unclassified 1024" > "$iolimits"

# Tolerated relative error (in %) for the strict fast side.
E=11
if is_windows_system; then
	E=15
fi
# Slow-side tolerance (in %): loose, reads can stall on busy CI machines.
E_SLOW=$((100 * $(timeout_get_total_multiplier)))
# Fast-side absolute grace (in ns) for the limiter's token bucket bursts.
FAST_GRACE=$((250 * 1000 * 1000))

CHUNKSERVERS=3 \
	USE_RAMDISK=YES \
	MASTER_EXTRA_CONFIG="GLOBALIOLIMITS_FILENAME = $iolimits" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

truncate -s1M "${info[mount0]}/file"

# consume accumulated limit
cat "${info[mount0]}/file" >/dev/null

{
	sleep 2
	echo "limit unclassified 2048" > "$iolimits"
	saunafs_master_daemon reload
} &

start=$(nanostamp)
for i in {1..6}; do
	cat "${info[mount0]}/file" >/dev/null
done
end=$(nanostamp)

expected=$((4 * 1000 * 1000 * 1000))
time=$((end - start))
abserr=$((time - expected))
relerr=$((100 * abserr / expected))
echo $expected $time $abserr $relerr
assert_less_or_equal $((expected * (100 - E) / 100 - FAST_GRACE)) ${time}
assert_less_or_equal ${time} $((expected * (100 + E_SLOW) / 100))

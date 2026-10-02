# Shows what the WAN options are worth. A long listing asks the filesystem for
# every file's extended attributes so it can print the trailing '+', which over a
# link with real latency turns one listing into one round trip per file. The tuned
# mount answers those requests itself, so the listing finishes in a time the
# default mount cannot reach once a delay is in place.

files=60
delay_ms=15
# Generous for a listing that makes no attribute requests, unreachable for one that
# makes a round trip per file at the delay injected below.
budget_ms=$(( 20 * 2 * delay_ms ))

tc_bin="$(command -v tc || echo /usr/sbin/tc)"
delay_applied=""

remove_delay() {
	if [[ $delay_applied ]]; then
		"$tc_bin" qdisc del dev lo root 2>/dev/null || true
		delay_applied=""
	fi
}

# Called by the harness if an assertion below fails. The EXIT trap covers the
# rest, including the interrupt the harness handles without this hook.
test_error_cleanup() {
	remove_delay
}
trap remove_delay EXIT

# Requests the client had to send to the master for extended attributes. A
# listing shows up as one of these per file, with a zero name length.
count_xattr_requests() {
	grep -c "master.cltoma_fuse_getxattr:" "$TEMP_DIR/master.log" 2>/dev/null || true
}

# Fails if the command does, so a broken mount cannot pass as a fast one.
elapsed_ms() {
	local start_ns end_ns status=0
	start_ns=$(date +%s%N)
	"$@" > /dev/null 2>&1 || status=$?
	end_ns=$(date +%s%N)
	echo $(( (end_ns - start_ns) / 1000000 ))
	return $status
}

CHUNKSERVERS=1 \
	MOUNTS=2 \
	USE_RAMDISK=YES \
	MASTER_EXTRA_CONFIG="MAGIC_DEBUG_LOG = $TEMP_DIR/master.log|LOG_FLUSH_ON=TRACE" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	MOUNT_1_EXTRA_CONFIG="sfswan" \
	setup_local_empty_saunafs info

default="${info[mount0]}"
wan="${info[mount1]}"

cd "$default"
mkdir dir
for i in $(seq 1 $files); do
	touch "dir/file$i"
done

# Let the entry, attribute and ACL caches expire so both listings start cold.
sleep 3

# Without the options, one listing asks the master about every file.
truncate -s0 "$TEMP_DIR/master.log"
ls -l "$default/dir" > /dev/null
default_requests=$(count_xattr_requests)

# With them, the client answers without asking the master at all.
truncate -s0 "$TEMP_DIR/master.log"
ls -l "$wan/dir" > /dev/null
wan_requests=$(count_xattr_requests)

assert_less_than $(( files / 2 )) "$default_requests"
assert_equals 0 "$wan_requests"

# Now put a delay on the loopback so those round trips cost what they would over a
# VPN, and compare how long the same listing takes. This measures the preset as a
# whole, the longer directory entry cache included; the counts above are what
# isolate the attribute traffic. Installing the delay needs privileges a
# restricted runner may not have, and the counts stand on their own without it.
if "$tc_bin" qdisc add dev lo root netem delay "${delay_ms}ms" 2>/dev/null; then
	delay_applied=yes
fi

if [[ $delay_applied ]]; then
	sleep 3
	default_ms=$(elapsed_ms ls -l "$default/dir")
	wan_ms=$(elapsed_ms ls -l "$wan/dir")
	remove_delay

	# The tuned listing finishes within the budget; the default one pays for its
	# per-file round trips and cannot.
	assert_less_than "$wan_ms" "$budget_ms"
	assert_less_than "$budget_ms" "$default_ms"
	assert_less_than $(( wan_ms * 3 )) "$default_ms"
else
	remove_delay
	echo "note: could not install a netem delay on lo, skipped the timing comparison"
fi

timeout_set '1 minute'

# Counterpart to the registration-window family: the same cluster state that
# those tests create TEMPORARILY -- an EC(2,1) chunk with too few parts to be
# used -- but this time it is permanent, because the chunkservers holding the
# other parts are stopped for good rather than restarted.
#
# Client IO on such a chunk must FAIL, promptly. Holding a request back and
# resolving it through the on-demand location query is only correct while parts
# are still arriving; when they never will, the master has to fall through to
# the error instead of parking the client indefinitely. The on-demand query is
# bounded on three sides for this reason: it is skipped when no connected
# chunkserver can answer, skipped once too many queries are already pending,
# and resolved anyway when it times out.
#
# So this test guards the failure direction of the same mechanism the
# registration-window tests guard the success direction of: a change that made
# deferring more eager would turn a fast, correct error into a hang, and that
# would show up here rather than in a green suite.

FILE_COUNT=${FILE_COUNT:-10}
FILE_SIZE=${FILE_SIZE:-262144}
# Generous: this bounds "does not hang", not the exact error latency, which is
# a few client retries plus at most one query timeout.
MAX_FAIL_SECONDS=60

CHUNKSERVERS=3 \
	DISK_PER_CHUNKSERVER=1 \
	MOUNTS=1 \
	USE_RAMDISK=YES \
	AUTO_SHADOW_MASTER=NO \
	MASTER_CUSTOM_GOALS="10 ec21: \$ec(2,1)" \
	CHUNKSERVER_EXTRA_CONFIG="MASTER_RECONNECTION_DELAY = 1" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER,sfsioretries=3" \
	setup_local_empty_saunafs info

file_path() {
	echo "${info[mount0]}/ec21/$(printf 'file_%05d' "$1")"
}

ec_dir="${info[mount0]}/ec21"
mkdir "$ec_dir"
assert_success saunafs setgoal ec21 "$ec_dir"

for ((i = 0; i < FILE_COUNT; ++i)); do
	FILE_SIZE=$FILE_SIZE file-generate "$(file_path "$i")"
done
probe_file=$(file_path 0)
MESSAGE="every chunk must start with all 3 parts known" \
	assert_eventually_equals "echo 3" "count_registered_parts '$probe_file'"
echo "PHASE: $FILE_COUNT EC(2,1) chunks created, 3 parts each"

# Drop every client-side cache so the probes below must consult the master
saunafs_mount_unmount 0
saunafs_mount_start 0

# Stop two chunkservers for good. One part survives -- one short of what
# EC(2,1) needs -- and no registration is in progress, so nothing will ever
# supply the rest.
for csid in 1 2; do
	saunafs_chunkserver_daemon "$csid" stop
done
MESSAGE="the chunk must be left below the EC(2,1) threshold, permanently" \
	assert_eventually_equals "echo 1" "count_registered_parts '$probe_file'"
echo "PHASE: chunkservers 1 and 2 stopped, chunk down to 1 of 3 parts for good"

# A read must fail rather than hang: the master consults the surviving
# chunkserver once, learns nothing new, and answers with what it has.
#
# The whole file is read on purpose. EC(2,1) stripes the data over two data
# parts, so a short read at a fixed offset only needs whichever part covers
# that range -- and whether the surviving chunkserver happens to hold it varies
# between runs. Such a probe measures part placement rather than availability
# and passes or fails at random. Reading everything forces the reconstruction
# that a single part cannot satisfy.
read_probe() {
	dd if="$probe_file" of=/dev/null bs=1M status=none
}
read_start_ns=$(date +%s%N)
MESSAGE="read of a permanently incomplete chunk must fail, not hang" \
	assert_failure read_probe
read_ms=$(( ($(date +%s%N) - read_start_ns) / 1000000 ))

# And so must a write
write_probe() {
	dd if=/dev/zero of="$probe_file" bs=4096 count=1 conv=notrunc,fsync status=none
}
write_start_ns=$(date +%s%N)
MESSAGE="write to a permanently incomplete chunk must fail, not hang" \
	assert_failure write_probe
write_ms=$(( ($(date +%s%N) - write_start_ns) / 1000000 ))

echo "LOST_CHUNK_READ_FAIL_MS: $read_ms"
echo "LOST_CHUNK_WRITE_FAIL_MS: $write_ms"

assert_less_than "$read_ms" "$((MAX_FAIL_SECONDS * 1000))"
assert_less_than "$write_ms" "$((MAX_FAIL_SECONDS * 1000))"

# Bringing the chunkservers back makes the data usable again, which confirms
# the failures above were about availability and not about the data itself
for csid in 1 2; do
	saunafs_chunkserver_daemon "$csid" start
done
saunafs_wait_for_all_ready_chunkservers
MESSAGE="the chunk must be complete again once the chunkservers return" \
	assert_eventually_equals "echo 3" "count_registered_parts '$probe_file'"

# Both operations that failed while the chunk was incomplete must work again,
# on that very same chunk. Order matters: the read is checked against the
# original generated content, so it has to happen before the write replaces
# part of it.
MESSAGE="read of the recovered chunk must succeed and return the original data" \
	assert_success file-validate "$probe_file"
MESSAGE="write to the recovered chunk must succeed" \
	assert_success write_probe
MESSAGE="the recovered chunk must read back what was just written" \
	assert_success cmp -n 4096 "$probe_file" /dev/zero

# A file nobody touched still validates, so the outage cost no data
assert_success file-validate "$(file_path 1)"

timeout_set 3 minutes

CHUNKSERVERS=3 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

# A size cached before a write must never be served after that write has been closed.
# Reopening per record is what matters: it releases the client write record, so the mount
# has nothing left to correct a stale size with.
cat >"${TEMP_DIR}/size_after_close.py" <<'END_OF_SOURCE'
import os
import sys
import threading
import time

path = sys.argv[1]
duration = float(sys.argv[2])
readers = int(sys.argv[3])
record = 4096
limit = 4 * 1024 * 1024

# One writer appends fixed size records to a file it recreates once it reaches the limit,
# reopening the file for every record. Readers keep checking the last committed record.
# "generation" counts recreations so a reader can tell a recreated file from a stale one,
# "committed" is the size the writer has closed so far, "total" never resets.
stop = threading.Event()
state = {"generation": 0, "committed": 0, "total": 0}
lock = threading.Lock()
stale_reads = []
stale_sizes = []
# Errors that say nothing about the cache, such as ENOSPC; they make the run inconclusive.
environment_errors = []

def writer():
    generation = 0
    while not stop.is_set():
        with lock:
            state["generation"] = generation
            state["committed"] = 0
        try:
            os.close(os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644))
        except OSError as error:
            environment_errors.append("create: %s" % error)
            stop.set()
            return

        offset = 0
        while not stop.is_set() and offset < limit:
            try:
                handle = os.open(path, os.O_WRONLY)
                written = os.pwrite(handle, bytes([(offset // record) % 251]) * record, offset)
                os.close(handle)
            except OSError as error:
                environment_errors.append("write: %s" % error)
                stop.set()
                return
            if written != record:
                environment_errors.append("short write of %d bytes" % written)
                stop.set()
                return
            offset += record
            with lock:
                state["committed"] = offset
                state["total"] += 1

        generation += 1
        try:
            os.unlink(path)
        except OSError:
            pass

def reader(identifier):
    # Each pass: note what the writer has committed, then stat and read the last record. Both
    # the reported size and the bytes read must reflect that committed size, because the writer
    # closed its handle before publishing it.
    while not stop.is_set():
        with lock:
            generation = state["generation"]
            committed = state["committed"]
        if committed < record:
            continue

        offset = committed - record
        try:
            reported = os.stat(path).st_size
            handle = os.open(path, os.O_RDONLY)
            data = os.pread(handle, record, offset)
            os.close(handle)
        except OSError as error:
            # ENOENT or ESTALE: the writer recreated the file between our stat and open.
            if error.errno in (2, 116):
                continue
            environment_errors.append("reader %d: %s" % (identifier, error))
            stop.set()
            return

        # The file was recreated under us, so this sample says nothing. Otherwise the file has
        # only grown since we sampled "committed", so a shorter answer is a stale one.
        with lock:
            if state["generation"] != generation:
                continue

        if len(data) != record:
            stale_reads.append((identifier, offset, len(data), committed))
            stop.set()
            return
        if reported < committed:
            stale_sizes.append((identifier, reported, committed))
            stop.set()
            return

threads = [threading.Thread(target=writer)]
threads += [threading.Thread(target=reader, args=(index,)) for index in range(readers)]
for thread in threads:
    thread.start()

deadline = time.time() + duration
while time.time() < deadline and not stop.is_set():
    time.sleep(0.1)
stop.set()
for thread in threads:
    thread.join()

try:
    os.unlink(path)
except OSError:
    pass

for entry in stale_reads[:5]:
    print("short read: reader=%s offset=%s got=%s committed=%s" % entry)
for entry in stale_sizes[:5]:
    print("stale size: reader=%s reported=%s committed=%s" % entry)
for entry in environment_errors[:5]:
    print("environment error: %s" % entry)

if environment_errors:
    sys.exit(2)
if stale_reads or stale_sizes:
    sys.exit(1)
print("clean records=%d generations=%d" % (state["total"], state["generation"]))
END_OF_SOURCE

cd "${info[mount0]}"
mkdir size_after_close
cd size_after_close

lookups_cached_before=$(awk '/lookup-cached/ {print $2}' "${info[mount0]}/.stats")

for attempt in 1 2 3; do
	python3 "${TEMP_DIR}/size_after_close.py" "$(realpath probe)" 20 12 \
		| tee "${TEMP_DIR}/probe_out_${attempt}"
	status=${PIPESTATUS[0]}
	if (( status == 2 )); then
		test_fail "Probe could not run, see the environment error above"
	fi
	if (( status == 1 )); then
		test_fail "A size cached before a write was served after that write was closed"
	fi

	# A clean run only means something if the workload actually ran.
	# Guard against a probe that died early, not a throughput requirement: CI runs this suite
	# with many workers and the write rate per container varies a lot.
	records=$(sed -n 's/.*clean records=\([0-9]*\).*/\1/p' "${TEMP_DIR}/probe_out_${attempt}")
	if [[ -z "${records}" ]] || (( records < 20 )); then
		test_fail "Probe wrote only '${records}' records, too few to exercise the invariant"
	fi
done

# The defect lives in the direntry cache, so a pass proves nothing if the cache never served a
# lookup during the run.
lookups_cached_after=$(awk '/lookup-cached/ {print $2}' "${info[mount0]}/.stats")
if (( lookups_cached_after <= lookups_cached_before )); then
	test_fail "No cached lookups were served, the direntry cache was not exercised"
fi

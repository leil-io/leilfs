timeout_set 1 minute

# Reproduces stale reads served from the mount-side read cache after truncate.
# The read cache is populated through an open descriptor, then the file is
# truncated (shrunk and sparsely re-grown, so its correct content is zeros).
# A pread on the same descriptor must not return the pre-truncate content.
# cacheexpirationtime (24h) far exceeds any scaled test timeout, so cache
# entries cannot expire mid-test and mask the regression with a silent pass.

CHUNKSERVERS=1 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER | cacheexpirationtime=86400000" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

FILE_SIZE=262144 file-generate file

cat > "$TEMP_DIR/stale_read_check.py" << 'END'
import os
import sys

path = sys.argv[1]
size = int(sys.argv[2])

fd = os.open(path, os.O_RDONLY)

before = os.pread(fd, size, 0)
if len(before) != size:
    print(f"SETUP FAILURE: initial read returned {len(before)} bytes, expected {size}")
    sys.exit(1)

os.truncate(path, 0)
os.truncate(path, size)  # sparse re-grow: correct content is all zeros

after = os.pread(fd, size, 0)
os.close(fd)

if after == b"\0" * size:
    sys.exit(0)

if len(after) != size:
    print(f"SHORT READ: pread returned {len(after)} bytes, expected {size}")
elif after == before:
    print("STALE READ: pread returned the pre-truncate content")
else:
    zeros = sum(b == 0 for b in after)
    print(f"CORRUPT READ: {zeros}/{size} bytes zero, "
          "neither all-zeros nor the pre-truncate content")
sys.exit(1)
END

assert_success python3 "$TEMP_DIR/stale_read_check.py" "$(realpath file)" 262144

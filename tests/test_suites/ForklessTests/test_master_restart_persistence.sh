timeout_set 2 minutes

# Repeated master restarts against the forkless (FoundationDB) metadata backend.
#
# A forkless master loads no metadata image on start: it reads the live FDB keyspace and rolls each
# section back to the last sealed checkpoint. Restarting twice in a row exercises that path against
# a keyspace the previous instance wrote, so a row that was never persisted, or a checkpoint that
# cannot be reloaded, changes the namespace the master serves.
#
# The namespace captured before the first restart is compared after each one. metadata_print()
# reports inode, type, goal, trash time, extra attributes, directory stats and per-chunk ids and
# versions, so a chunk whose CHNK_ row was lost, or whose id generator drifted after rollback,
# shows up as a difference rather than passing unnoticed.

CHUNKSERVERS=5 \
	METADATA_BACKEND="FORKLESS" \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	SFSEXPORTS_EXTRA_OPTIONS="allcanchangequota,ignoregid" \
	setup_local_empty_saunafs info

cd ${info[mount0]}

# Create a 1MiB file to generate some chunks
dd if=/dev/urandom of=file.1MiB bs=1M count=1 status=none

# Capture the namespace the master serves before any restart. The chunk assertion keeps the
# comparisons below from passing on two empty captures.
metadata=$(metadata_print)
assert_awk_finds '/chunk 0:/' "$metadata"

# First restart: the master rebuilds its image from FDB instead of reading metadata.sfs.
saunafs_master_daemon restart
saunafs_wait_for_all_ready_chunkservers
assert_no_diff "$metadata" "$(metadata_print)"

# Second restart: the keyspace now also holds whatever the restarted master wrote on shutdown.
saunafs_master_daemon restart
saunafs_wait_for_all_ready_chunkservers
assert_no_diff "$metadata" "$(metadata_print)"

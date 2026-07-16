timeout_set 2 minutes

# Regression test: a disk removed from the hdd configuration must stop
# receiving new chunks as soon as the chunkserver reloads its configuration.
#
# The chunkserver handles a config-removed disk in two asynchronous steps:
# the reload only marks the disk as removed, and the once-per-second disk
# check (hddCheckDisks) later erases its chunks from the registry and drops
# the disk. If new chunks are still allocated on the marked disk inside that
# window, the disk check erases them while clients are using them, the master
# reports them as lost and the data is gone (write errors ending in EIO).
#
# To hit the window deterministically the test synchronizes with the disk
# check tick using a sacrificial disk: a removed disk disappears from
# `saunafs-admin list-disks` exactly when the tick drops it from the disk
# registry, so observing that drop marks a tick boundary and the whole next
# second belongs to the removal window of a disk removed right after.
#
# The removal is repeated for several cycles. A cycle whose window collapsed
# (the disk check tick fired before the writes finished, e.g. on a heavily
# loaded machine) is skipped instead of failed; the regression assertion runs
# only in cycles whose window provably held, and at least one such cycle is
# required. Inside a valid window the bug reproduces deterministically, since
# chunk allocation alternates between the eligible disks.

USE_RAMDISK=YES \
	CHUNKSERVERS=1 \
	DISK_PER_CHUNKSERVER=3 \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

list_disks_count() {
	saunafs-admin list-disks --porcelain localhost "${info[matocl]}" | wc -l
}

# Print the filesystem path of a disk from the chunkserver 0 hdd config,
# stripping comment markers, the optional zonefs: prefix, the
# marked-for-removal asterisk and the "| dataPath" part of zoned entries.
get_disk_path() {
	local disk_id=$1
	sed -n "$((disk_id + 1))p" "${info[chunkserver0_hdd]}" | sed -E \
		-e 's/^[[:space:]#]*//' \
		-e 's/^zonefs://' \
		-e 's/^\*//' \
		-e 's/[[:space:]]*\|.*$//' \
		-e 's/[[:space:]]*$//'
}

target_disk_path=$(get_disk_path 1)

cd "${info[mount0]}"

assert_eventually_prints 3 "list_disks_count" "30 seconds"

valid_cycles=0

for cycle in 1 2 3 4 5; do
	# Tick synchronization: remove the sacrificial disk (id 2) and wait for
	# it to disappear from list-disks. The moment the count drops from 3 to 2
	# is a disk check tick, so the next tick is a full second away; the 0.1 s
	# polling of assert_eventually_prints detects the drop well inside that
	# window.
	assert_success disable_chunkserver_disk 0 2
	assert_eventually_prints 2 "list_disks_count" "30 seconds"

	# Remove the target disk right after the observed tick.
	assert_success disable_chunkserver_disk 0 1

	# Give the chunkserver event loop time to process the reload (50 ms poll
	# timeout), so the target disk is already marked as removed from the
	# configuration before any file is created.
	sleep 0.3

	# Create new files, ideally well inside the removal window.
	for i in {1..8}; do
		FILE_SIZE=1K assert_success file-generate "cycle${cycle}_file_${i}"
	done

	if [[ "$(list_disks_count)" == "2" ]]; then
		# The target disk is still listed, which proves the disk check tick
		# has not fired yet and all files above were created inside the
		# removal window: the regression assertion is meaningful.
		valid_cycles=$((valid_cycles + 1))

		# The actual regression check: no chunk of the newly created files
		# may be allocated on the disk that was already removed from the
		# configuration. -type f excludes the chunksXX subfolder directories.
		chunks_on_removed_disk=$(find "${target_disk_path}" -type f -name 'chunk*' | wc -l)
		MESSAGE="cycle ${cycle}: chunks allocated on a disk removed from config" \
			assert_equals 0 "${chunks_on_removed_disk}"
		echo "cycle ${cycle}: removal window held, regression assertion checked"
	else
		# The tick fired before the writes finished: the machine broke the
		# timing assumptions of this cycle, so the assertion would be
		# inconclusive. Skip it and retry on the next cycle.
		echo "cycle ${cycle}: removal window collapsed, skipping the check"
	fi

	# Wait for the disk check tick to drop the target disk, then restore
	# both disks for the next cycle.
	assert_eventually_prints 1 "list_disks_count" "30 seconds"
	assert_success reenable_chunkserver_disk 0 1
	assert_success reenable_chunkserver_disk 0 2
	assert_eventually_prints 3 "list_disks_count" "30 seconds"
done

MESSAGE="no cycle kept its removal window; timing assumptions broken on this machine" \
	assert_less_than 0 "${valid_cycles}"

# All files must be complete and readable from the disk that stayed in the
# configuration for the whole test.
for cycle in 1 2 3 4 5; do
	for i in {1..8}; do
		assert_success file-validate "cycle${cycle}_file_${i}"
	done
done

# Under the FDB backend every lock and unlock is a durable transaction, so this
# 100k-operation ping-pong runs far longer than on the in-memory master (which
# applies locks in memory). Give the FDB backend a larger budget; the in-memory
# master keeps the historical timeout.
if [[ "${METADATA_BACKEND:-}" == "FDB" ]]; then
	timeout_set '10 minutes'
else
	timeout_set '1 minute'
fi

USE_RAMDISK=YES \
	MOUNTS=5
	MOUNT_EXTRA_CONFIG="enablefilelocks=1" \
	setup_local_empty_saunafs info

touch ${info[mount0]}/lockfile

# Launch ping pong instances
for i in {0..4}; do
	cd ${info[mount$i]}
	safs_ping_pong lockfile 6&
	ping_pongs[$i]=$!
done

# Ensure that all ping pong tests finished with no errors
for i in {0..4}; do
	wait ${ping_pongs[$i]}
	assert_equals 0 $?
done

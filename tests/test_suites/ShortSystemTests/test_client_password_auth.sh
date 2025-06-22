timeout_set "2 minutes"

USE_RAMDISK=YES \
    MOUNT_EXTRA_CONFIG="sfspassword=samplepassword,sfsargon2pass=1" \
	SFSEXPORTS_EXTRA_OPTIONS="password=samplepassword,argon2pass" \
    setup_local_empty_saunafs info

# Ensure client can be mounted
assert_success dd if=/dev/zero of="${info[mount0]}/.mount_test.bin" bs=1M count=1 oflag=sync

sleep 5

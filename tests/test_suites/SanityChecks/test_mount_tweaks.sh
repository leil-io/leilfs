#
# To run this test you need to add the following lines to /etc/sudoers.d/saunafstest:
#
# saunafstest ALL = NOPASSWD: /usr/bin/tee /tmp/SaunaFS-autotests/mnt/sfs*
#
#
MOUNTS=2 \
  USE_RAMDISK=YES \
  MOUNT_0_EXTRA_CONFIG="sfsdirectio=0" \
  MOUNT_1_EXTRA_CONFIG="sfsdirectio=1" \
  setup_local_empty_saunafs info

expect_equals "false" "$(cat ${info[mount0]}/.saunafs_tweaks | grep -i directio | awk '{print $2}')"
expect_equals "true" "$(cat ${info[mount1]}/.saunafs_tweaks | grep -i directio | awk '{print $2}')"

# Modify the options on the fly from tweaks file
echo "WriteWindowSize=50" | sudo tee ${info[mount0]}/.saunafs_tweaks
echo "UseWriteFlushPacket=true" | sudo tee ${info[mount0]}/.saunafs_tweaks
echo "DirectIO=true" | sudo tee ${info[mount0]}/.saunafs_tweaks
echo "DirectIO=false" | sudo tee ${info[mount1]}/.saunafs_tweaks

expect_equals "50" "$(cat ${info[mount0]}/.saunafs_tweaks | grep -i WriteWindowSize | awk '{print $2}')"
expect_equals "true" "$(cat ${info[mount0]}/.saunafs_tweaks | grep -i UseWriteFlushPacket | awk '{print $2}')"
expect_equals "true" "$(cat ${info[mount0]}/.saunafs_tweaks | grep -i directio | awk '{print $2}')"
expect_equals "false" "$(cat ${info[mount1]}/.saunafs_tweaks | grep -i directio | awk '{print $2}')"

timeout_set "2 minutes"
assert_program_installed openssl

# Set up temp directory for certs
echo "Generating TLS certificates for master-client communication test..."
TLS_CERTS_DIR=${TEMP_DIR}/testcerts
generate_certs ${TLS_CERTS_DIR}

echo "generating TLS certificates for reload TLS config testing for master-client communication test..."
TLS_CERTS_DIR_RELOAD=${TEMP_DIR}/testcerts_reload
generate_certs ${TLS_CERTS_DIR_RELOAD}

# Create TLS config file pointing to reload TLS config
cat > ${TEMP_DIR}/sfstls.cfg <<EOF
tlscertfile=${TLS_CERTS_DIR}/client.crt
tlskeyfile=${TLS_CERTS_DIR}/client.key
tlsservercacertfile=
tlsisserver=false
tlsexpectedhostname=sfsmaster
EOF

# Generate a second CA and client certs (invalid for master trust)
INVALID_TLS_CERTS_DIR=${TEMP_DIR}/invalid_testcerts
generate_certs ${INVALID_TLS_CERTS_DIR}

# Create TLS config file pointing to reload invalid TLS config
cat > ${TEMP_DIR}/sfstls_invalid.cfg <<EOF
tlscertfile=${INVALID_TLS_CERTS_DIR}/client.crt
tlskeyfile=${INVALID_TLS_CERTS_DIR}/client.key
tlsservercacertfile=${INVALID_TLS_CERTS_DIR}/ca.crt
tlsisserver=false
tlsexpectedhostname=sfsmaster
EOF

# Configure master to use TLS certs
master_cfg="|TLS_CERT_FILE = ${TLS_CERTS_DIR}/server.crt"
master_cfg+="|TLS_KEY_FILE = ${TLS_CERTS_DIR}/server.key"
master_cfg+="|TLS_CA_CERT_FILE = ${TLS_CERTS_DIR}/ca.crt"

# Set environment variable for client to find CA cert and check that
# expected logic trying to use value from SSL_CERT_FILE when no CA
# is given in mount configuration works.
export SSL_CERT_FILE="${TLS_CERTS_DIR}/ca.crt"

USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="tlsconfigfile=${TEMP_DIR}/sfstls.cfg" \
	MASTER_EXTRA_CONFIG="$master_cfg" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

# 1) Create multiple files to test TLS communication
for i in {1..5}; do
    touch file_tls_$i.txt
done

# Verify that all files were created successfully
assert_equals "5" "$(ls | grep '^file_tls_' | wc -l)"

# 2) Try mounting client with correct certs again but with CA given by configuration
cd ..

saunafs_mount_unmount 0

# Use new mount config with TLS CA file value manually set
sed -i \
  "s|^tlsservercacertfile=.*|tlsservercacertfile=${TLS_CERTS_DIR}/ca.crt|" \
  "${TEMP_DIR}/sfstls.cfg"

saunafs_mount_start 0 &

# Wait for the mount process to complete before accessing the mount point
sleep 1

cd "${info[mount0]}"

# Verify that all previously created files are accessible
assert_equals "5" "$(ls | grep '^file_tls_' | wc -l)"

# 3) Now restart master with new TLS certs, then update client TLS config
# on runtime and then check that reconnection is OK
cd ..

saunafs_mount_unmount 0

tmp_cfg="${TEMP_DIR}/master.cfg.new"

# Create a modified copy
sed \
  -e "s|^TLS_CERT_FILE *=.*|TLS_CERT_FILE = ${TLS_CERTS_DIR_RELOAD}/server.crt|" \
  -e "s|^TLS_KEY_FILE *=.*|TLS_KEY_FILE = ${TLS_CERTS_DIR_RELOAD}/server.key|" \
  -e "s|^TLS_CA_CERT_FILE *=.*|TLS_CA_CERT_FILE = ${TLS_CERTS_DIR_RELOAD}/ca.crt|" \
  "${info[master_cfg]}" > "$tmp_cfg"

# Atomically replace the original file (same path)
mv "$tmp_cfg" "${info[master_cfg]}"

# Explicit reload of master
saunafs_master_daemon reload
saunafs_wait_for_all_ready_chunkservers

# Use new mount config via config file to ensure it tries first with new files
rm "${TEMP_DIR}/sfstls.cfg"
cat > ${TEMP_DIR}/sfstls.cfg <<EOF
tlscertfile=${TLS_CERTS_DIR_RELOAD}/client.crt
tlskeyfile=${TLS_CERTS_DIR_RELOAD}/client.key
tlsservercacertfile=${TLS_CERTS_DIR_RELOAD}/ca.crt
tlsisserver=false
tlsexpectedhostname=sfsmaster
EOF

# Start the client again
saunafs_mount_start 0

# Now check that filesystem started
cd "${info[mount0]}"
assert_equals "5" "$(ls | grep '^file_tls_' | wc -l)"

# Check TLS config file value from Tweaks file.
expect_equals "${TEMP_DIR}/sfstls.cfg" "$(cat ${info[mount0]}/.saunafs_tweaks | grep -i tlsconfigfile | awk '{print $2}')"

# Copy TLS client config file and update through Tweaks to force runtime update and reconnection
cp ${TEMP_DIR}/sfstls.cfg ${TEMP_DIR}/sfstls_copy.cfg
echo "TlsConfigFile=${TEMP_DIR}/sfstls_copy.cfg" | sudo tee ${info[mount0]}/.saunafs_tweaks
# Wait a bit to allow reconnection to happen
sleep 3

# Check new updated value, which confirms reconnection was performed and update was correct
expect_equals "${TEMP_DIR}/sfstls_copy.cfg" "$(cat ${info[mount0]}/.saunafs_tweaks | grep -i tlsconfigfile | awk '{print $2}')"

# 4) Now test that with invalid certs, client will not mount and it
cd ..

saunafs_mount_unmount 0

echo "tlsconfigfile=${TEMP_DIR}/sfstls_invalid.cfg" >> "${info[mount0_cfg]}"

saunafs_mount_start 0 &

# Wait for the mount process to attempt and fail before accessing the mount point
sleep 1

cd "${info[mount0]}"

assert_failure cat .saunafs_mount_info

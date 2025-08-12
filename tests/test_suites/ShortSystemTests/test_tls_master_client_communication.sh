timeout_set "2 minutes"
assert_program_installed openssl

# Set up temp directory for certs
echo "Generating TLS certificates for master-client communication test..."
TLS_CERTS_DIR=${TEMP_DIR}/testcerts
generate_certs ${TLS_CERTS_DIR}

# Generate a second CA and client certs (invalid for master trust)
INVALID_TLS_CERTS_DIR=${TEMP_DIR}/invalid_testcerts
generate_certs ${INVALID_TLS_CERTS_DIR}

# Configure master to use TLS certs
master_cfg="|TLS_CERT_FILE = ${TLS_CERTS_DIR}/server.crt"
master_cfg+="|TLS_KEY_FILE = ${TLS_CERTS_DIR}/server.key"
master_cfg+="|TLS_CA_CERT_FILE = ${TLS_CERTS_DIR}/ca.crt"

# Set environment variable for client to find CA cert and check that
# expected logic trying to use value from SSL_CERT_FILE when no CA
# is given in mount configuration works.
export SSL_CERT_FILE="${TLS_CERTS_DIR}/ca.crt"

USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="tlscertfile=${TLS_CERTS_DIR}/client.crt,tlskeyfile=${TLS_CERTS_DIR}/client.key" \
	MASTER_EXTRA_CONFIG="$master_cfg" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

# Create multiple files to test TLS communication
for i in {1..5}; do
    touch file_tls_$i.txt
done

# Verify that all files were created successfully
assert_equals "5" "$(ls | grep '^file_tls_' | wc -l)"

# Try mounting client with correct certs again but with CA given by configuration
cd ..

saunafs_mount_unmount 0

# Use new mount config with TLS CA file
echo "tlscertfile=${TLS_CERTS_DIR}/client.crt,tlskeyfile=${TLS_CERTS_DIR}/client.key,tlsservercacertfile=${TLS_CERTS_DIR}/ca.crt" >> "${info[mount0_cfg]}"

saunafs_mount_start 0 &

# Wait for the mount process to complete before accessing the mount point
sleep 1

cd "${info[mount0]}"

# Verify that all previously created files are accessible
assert_equals "5" "$(ls | grep '^file_tls_' | wc -l)"

# Now test that with invalid certs, client will not mount and it
cd ..

saunafs_mount_unmount 0

echo "tlscertfile=${INVALID_TLS_CERTS_DIR}/client.crt,tlskeyfile=${INVALID_TLS_CERTS_DIR}/client.key,tlsservercacertfile=${INVALID_TLS_CERTS_DIR}/ca.crt" >> "${info[mount0_cfg]}"

saunafs_mount_start 0 &

# Wait for the mount process to attempt and fail before accessing the mount point
sleep 1

cd "${info[mount0]}"

assert_failure cat .saunafs_mount_info

timeout_set "1 minutes"
assert_program_installed openssl

apply_chunkserver_config() {
	local parameter=$1
	local value=$2

	# Escape slashes and ampersands in the value for sed
	local escaped_value
	escaped_value=$(printf '%s\n' "$value" | sed 's/[\/&]/\\&/g')

	for i in $(seq 0 $(( ${info[chunkserver_count]} - 1 ))); do
		sed -i -re "s|^$parameter = .*|$parameter = $escaped_value|" "${info[chunkserver${i}_cfg]}"
	done
}

# Set up temp directory for certs
echo "Generating TLS certificates for master-chunkserver communication test..."
TLS_CERTS_DIR=${TEMP_DIR}/testcerts
generate_certs ${TLS_CERTS_DIR}

# Generate a second CA and certs (invalid for master trust)
INVALID_TLS_CERTS_DIR=${TEMP_DIR}/invalid_testcerts
generate_certs ${INVALID_TLS_CERTS_DIR}

# Create TLS config file pointing to reload TLS config on client side
cat > ${TEMP_DIR}/sfstls.cfg <<EOF
tlscertfile=${TLS_CERTS_DIR}/client.crt
tlskeyfile=${TLS_CERTS_DIR}/client.key
tlsservercacertfile=${TLS_CERTS_DIR}/ca.crt
tlsisserver=false
tlsexpectedhostname=sfsmaster
EOF

# Configure master to use TLS certs
master_cfg="|TLS_CERT_FILE = ${TLS_CERTS_DIR}/server.crt"
master_cfg+="|TLS_KEY_FILE = ${TLS_CERTS_DIR}/server.key"
master_cfg+="|TLS_CA_CERT_FILE = ${TLS_CERTS_DIR}/ca.crt"

# Configure chunkserver to use TLS certs
cs_cfg="|TLS_CERT_FILE = ${TLS_CERTS_DIR}/cs.crt"
cs_cfg+="|TLS_KEY_FILE = ${TLS_CERTS_DIR}/cs.key"
cs_cfg+="|TLS_CA_CERT_FILE = ${TLS_CERTS_DIR}/ca.crt"

# Set environment variable for client to find CA cert and check that
# expected logic trying to use value from SSL_CERT_FILE when no CA
# is given in mount configuration works.
export SSL_CERT_FILE="${TLS_CERTS_DIR}/ca.crt"

USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER,sfsioretries=5,tlsconfigfile=${TEMP_DIR}/sfstls.cfg" \
	MASTER_EXTRA_CONFIG="${master_cfg}" \
	CHUNKSERVER_EXTRA_CONFIG="${cs_cfg}" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

# Create multiple files with data to test TLS communication
for i in {1..5}; do
	echo "data" > file_tls_$i.txt
done

# Verify that all files were created successfully
assert_equals "5" "$(ls | grep '^file_tls_' | wc -l)"

# Verify file contents
for i in {1..5}; do
	assert_equals "data" "$(cat file_tls_$i.txt)"
done

# Try restarting chunkserver with incorrect certs (invalid for master trust)
# Update chunkserver config to use invalid certs
apply_chunkserver_config TLS_CERT_FILE "${INVALID_TLS_CERTS_DIR}/cs.crt"
apply_chunkserver_config TLS_KEY_FILE "${INVALID_TLS_CERTS_DIR}/cs.key"
apply_chunkserver_config TLS_CA_CERT_FILE "${INVALID_TLS_CERTS_DIR}/ca.crt"

# Restart chunkserver to apply new config
saunafs_chunkserver_daemon 0 restart

# Verify that accessing files contents fails due to chunkserver not starting
# because of invalid TLS certs
for i in {1..5}; do
	assert_failure cat "${info[mount0]}/file_tls_$i.txt"
done

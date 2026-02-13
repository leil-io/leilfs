timeout_set "2 minutes"
assert_program_installed openssl

# Parameters
password="good-password"
chunkservers=3
mounts=2

# Values to search for in dump-config
master_config_value="NO_ATIME = 0"
mount_config_value=sfscachemode=NEVER
chunk_config_value="READ_AHEAD_KB = 1024"

echo "Generating TLS certificates for master-client communication test..."
TLS_CERTS_DIR="${TEMP_DIR}/testcerts"
generate_certs "${TLS_CERTS_DIR}"

# Create TLS client config file for saunafs-admin and mounts
TLS_CFG_FILE="${TEMP_DIR}/sfstls.cfg"
cat > "${TLS_CFG_FILE}" <<EOF
tlscertfile=${TLS_CERTS_DIR}/client.crt
tlskeyfile=${TLS_CERTS_DIR}/client.key
tlsservercacertfile=${TLS_CERTS_DIR}/ca.crt
tlsisserver=false
tlsexpectedhostname=sfsmaster
EOF

# Configure master to use TLS certs
master_tls_cfg="|TLS_CERT_FILE = ${TLS_CERTS_DIR}/server.crt"
master_tls_cfg+="|TLS_KEY_FILE = ${TLS_CERTS_DIR}/server.key"
master_tls_cfg+="|TLS_CA_CERT_FILE = ${TLS_CERTS_DIR}/ca.crt"

# Bring up a local SaunaFS with TLS enabled on master and pass TLS config to mounts/admin
CHUNKSERVERS="${chunkservers}" \
  CHUNKSERVER_EXTRA_CONFIG="${chunk_config_value}" \
  MASTER_EXTRA_CONFIG="${master_config_value}${master_tls_cfg}" \
  MOUNT_EXTRA_CONFIG="tlsconfigfile=${TLS_CFG_FILE}|${mount_config_value}" \
  MOUNTS="${mounts}" \
  USE_RAMDISK="YES" \
  ADMIN_PASSWORD="${password}" \
  setup_local_empty_saunafs info

# Read ports exported by setup
master_port="${info[matocl]}"
shadow_port="${info[masterauto_matocl]}"

# 1) Verify saunafs-admin info works over TLS
saunafs-admin info --porcelain localhost "${master_port}" --tlsconfigfile="${TLS_CFG_FILE}"

# 2) Verify wrong password is rejected over TLS
assert_failure saunafs-admin dump-config localhost "${master_port}" --tlsconfigfile="${TLS_CFG_FILE}" <<< "wrong-password"

# 3) Verify correct password works over TLS and output contains expected entries
config="$(saunafs-admin dump-config localhost "${master_port}" --tlsconfigfile="${TLS_CFG_FILE}" <<< "${password}")"
printf "%s\n" "${config}"

# Master option present once
expect_equals 1 "$(grep "${master_config_value// = /: }" <<< "${config}" | wc -l)"

# Expect chunkserver entries (one per chunkserver)
expect_equals "${chunkservers}" "$(grep "${chunk_config_value// = /: }" <<< "${config}" | wc -l)"

# Find mount entries
expect_equals "${mounts}" "$(grep "${mount_config_value//=/: }" <<< "${config}" | wc -l)"

# 4) Verify shadow works over TLS (subset of config)
config="$(saunafs-admin dump-config localhost "${shadow_port}" --tlsconfigfile="${TLS_CFG_FILE}" <<< "${password}")"
printf "%s\n" "${config}"
expect_equals 1 "$(grep "${master_config_value// = /: }" <<< "${config}" | wc -l)"

# 5) Verify defaults dump over TLS
config="$(saunafs-admin dump-config --defaults localhost "${master_port}" --tlsconfigfile="${TLS_CFG_FILE}" <<< "${password}")"
printf "%s\n" "${config}"
# Spot-check a couple of defaults that should be stable
expect_equals 1 "$(grep "GLOBALIOLIMITS_RENEGOTIATION_PERIOD_SECONDS: 0.1" <<< "${config}" | wc -l)"
# This value may appear across multiple services, adjust expected count if needed
expect_equals 3 "$(grep "REPLICATION_BANDWIDTH_LIMIT_KBPS: 0" <<< "${config}" | wc -l)"

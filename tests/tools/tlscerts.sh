#!/usr/bin/env bash
#------------------------------------------------------------------------------
# TLS certificate generation helpers (sourceable)
#
# Provides:
#   generate_certs <output_dir>
#
# Environment variables:
#   CERT_DAYS   - Certificate validity in days (default: 1)
#   SERVER_DNS  - Server DNS SAN (default: sfsmaster)
#
# Requires:
#   - openssl
#   - get_ip_addr
#   - is_windows_system
#   - assert_program_installed
#------------------------------------------------------------------------------

generate_certs() {
	assert_program_installed openssl

	if [ -z "$1" ]; then
		echo "Usage: generate_certs <output_dir>" >&2
		return 1
	fi

	local output_dir="$1"

	local CERT_DAYS="${CERT_DAYS:-1}"
	local SERVER_DNS="${SERVER_DNS:-sfsmaster}"
	local SERVER_IP

	SERVER_IP="$(get_ip_addr)" || {
		echo "Failed to determine server IP" >&2
		return 1
	}

	(
		mkdir -p "$output_dir" || exit 1
		cd "$output_dir" || exit 1

		if [ -n "$(ls -A . 2>/dev/null)" ]; then
			echo "Directory '$output_dir' is not empty." >&2
			echo "Use --force to overwrite existing files." >&2
			exit 1
		fi

		rm -f \
			ca.key ca.crt ca.srl \
			server.key server.csr server.crt server_ext.cnf \
			client.key client.csr client.crt \
			cs.key cs.csr cs.crt

		# 1. Create CA key and certificate
		openssl genrsa -out ca.key 4096
		openssl req -x509 -new -nodes \
			-key ca.key \
			-sha256 \
			-days "$CERT_DAYS" \
			-out ca.crt \
			-subj "/CN=Test CA"

		# 2. Create server key and CSR
		openssl genrsa -out server.key 4096
		openssl req -new \
			-key server.key \
			-out server.csr \
			-subj "/CN=Test Server"

		# 3. Server certificate extensions (SANs)
		cat > server_ext.cnf <<EOF
basicConstraints = CA:FALSE
keyUsage = digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = @alt_names

[alt_names]
IP.1 = ${SERVER_IP}
DNS.1 = ${SERVER_DNS}
EOF

		# 4. Sign server CSR with CA (with SANs)
		openssl x509 -req \
			-in server.csr \
			-CA ca.crt \
			-CAkey ca.key \
			-CAcreateserial \
			-out server.crt \
			-days "$CERT_DAYS" \
			-sha256 \
			-extfile server_ext.cnf

		# 5. Create client key and CSR
		openssl genrsa -out client.key 4096
		openssl req -new \
			-key client.key \
			-out client.csr \
			-subj "/CN=Test Client"

		# 6. Sign client CSR with CA
		openssl x509 -req \
			-in client.csr \
			-CA ca.crt \
			-CAkey ca.key \
			-CAcreateserial \
			-out client.crt \
			-days "$CERT_DAYS" \
			-sha256

		# 7. Create chunkserver key and CSR
		openssl genrsa -out cs.key 4096
		openssl req -new \
			-key cs.key \
			-out cs.csr \
			-subj "/CN=Test Chunkserver"

		# 8. Sign chunkserver CSR with CA
		openssl x509 -req \
			-in cs.csr \
			-CA ca.crt \
			-CAkey ca.key \
			-CAcreateserial \
			-out cs.crt \
			-days "$CERT_DAYS" \
			-sha256

		# Windows / WSL compatibility
		if is_windows_system; then
			chmod -R 777 .
		fi

		echo "Certificates generated in: $output_dir"
		echo "Validity: ${CERT_DAYS} day(s)"
		echo "Server SANs: IP=${SERVER_IP}, DNS=${SERVER_DNS}"
	)
}

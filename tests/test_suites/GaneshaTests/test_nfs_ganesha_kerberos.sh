# Verify that Ganesha with the SaunaFS FSAL accepts Kerberos-authenticated NFSv4
# mounts for both explicit NFSv4.1 and NFSv4.2 requests. The test creates a
# temporary Kerberos realm, rejects AUTH_SYS, and validates krb5/krb5i/krb5p I/O.
#
# End-to-end flow covered by this test:
# 1. A local MIT Kerberos realm acts as the KDC and issues tickets.
# 2. The Linux NFS client mounts with sec=krb5, krb5i, or krb5p.
# 3. The kernel NFS client asks rpc.gssd for RPCSEC_GSS credentials via rpc_pipefs.
# 4. Ganesha validates those Kerberos-backed RPCs in its NFS/RPC layer.
# 5. The SaunaFS FSAL receives already-authenticated requests from Ganesha.
#
# Kerberos authentication is handled by the client/KDC/rpc.gssd/RPCSEC_GSS/Ganesha
# path before requests reach the SaunaFS FSAL.
# The FSAL does not process Kerberos tickets itself; it only receives the
# authenticated identity and request context that Ganesha has already validated.
#
# Separation of responsibilities:
# - KDC: issues Kerberos tickets.
# - Client kernel NFS: sends NFS RPCs.
# - rpc.gssd: bridges kernel NFS and Kerberos credentials.
# - rpc_pipefs: communication channel for kernel/userspace NFS auth upcalls.
# - Ganesha RPC/NFS layer: validates Kerberos, enforces SecType and manages NFSv4 sessions/state.
# - SaunaFS FSAL: executes filesystem operations against SaunaFS using the identity/context Ganesha provides.
#
# Useful links:
# - Using RPCSEC_GSS with NFS-GANESHA: https://github.com/nfs-ganesha/nfs-ganesha/wiki/RPCSEC_GSS
# - Kerberos setup for NFS-Ganesha in Ceph: https://github.com/nfs-ganesha/nfs-ganesha/wiki/Kerberos-setup-for-NFS-Ganesha-in-Ceph

timeout_set 2 minutes

find_free_tcp_ports() {
	local port_count=$1

	python3 - "${port_count}" <<'PY'
import socket
import sys

port_count = int(sys.argv[1])
server_sockets = []

try:
	for port_index in range(port_count):
		server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
		server_socket.bind(("127.0.0.1", 0))
		server_sockets.append(server_socket)

	print(" ".join(str(server_socket.getsockname()[1]) for server_socket in server_sockets))
finally:
	for server_socket in server_sockets:
		server_socket.close()
PY
}

get_mountstats_block() {
	local mountpoint=$1
	awk -v mountpoint="${mountpoint}" '
		$1 == "device" && index($0, " mounted on " mountpoint " ") { found = 1 }
		found { print }
		found && $0 == "" { exit }
	' /proc/self/mountstats
}

assert_mountstats_option() {
	local mountstats_block=$1
	local option_name=$2
	local option_value=$3

	if ! grep -Eq "(^|,)${option_name}=${option_value}(,|$)" <<< "${mountstats_block}"; then
		test_fail "Expected ${option_name}=${option_value} in mountstats:${mountstats_block}"
	fi
}

assert_program_installed python3 nc kinit klist kdestroy
kadmin_local_bin=$(find_program kadmin.local) || test_fail "kadmin.local is not installed"
kdb5_util_bin=$(find_program kdb5_util) || test_fail "kdb5_util is not installed"
krb5kdc_bin=$(find_program krb5kdc) || test_fail "krb5kdc is not installed"
rpc_gssd_bin=$(find_program rpc.gssd) || test_fail "rpc.gssd is not installed"

CHUNKSERVERS=3 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

readonly realm="SAUNAFS.TEST"
readonly client_principal="saunafstest"
readonly client_password="saunafs-test-password"
readonly master_password="saunafs-master-password"

mountpoint_path="${TEMP_DIR}/mnt/ganesha-krb5"
krb5_dir="${TEMP_DIR}/krb5"
rpc_pipefs="${TEMP_DIR}/rpc_pipefs"
ganesha_config="${info[mount0]}/ganesha-krb5.conf"
ganesha_log="${TEMP_DIR}/ganesha-krb5.log"
rpc_gssd_log="${TEMP_DIR}/rpc-gssd.log"
krb5_config="${krb5_dir}/krb5.conf"
kdc_config="${krb5_dir}/kdc.conf"
server_keytab="${krb5_dir}/ganesha.keytab"
credential_cache_dir="${krb5_dir}/ccaches"

# Keep separate caches for the regular test user and the sudo-driven mount path.
# The kernel mount helpers run under a privileged context, so this test prepares
# credentials explicitly for both sides instead of relying on a shared default cache.
credential_cache="${credential_cache_dir}/krb5cc_$(id -u)"
mount_credential_cache="${credential_cache_dir}/krb5cc_0"
kdc_pid=""
mounted_rpc_pipefs=NO

test_error_cleanup() {
	set +e
	if mountpoint -q "${mountpoint_path}"; then
		sudo umount -l "${mountpoint_path}"
	fi

	sudo pkill -9 ganesha.nfsd
	sudo pkill -f "rpc.gssd.*${rpc_pipefs}"

	if [[ -n ${kdc_pid} ]]; then
		kill "${kdc_pid}"
		wait "${kdc_pid}" 2>/dev/null
	fi

	KRB5_CONFIG="${krb5_config}" KRB5CCNAME="FILE:${credential_cache}" kdestroy 2>/dev/null
	KRB5_CONFIG="${krb5_config}" KRB5CCNAME="FILE:${mount_credential_cache}" \
		sudo --preserve-env=KRB5_CONFIG,KRB5CCNAME kdestroy 2>/dev/null

	if [[ ${mounted_rpc_pipefs} == YES ]] && mountpoint -q "${rpc_pipefs}"; then
		sudo umount -l "${rpc_pipefs}"
	fi
}
trap test_error_cleanup EXIT

mkdir -p "${mountpoint_path}" "${krb5_dir}" "${rpc_pipefs}" "${credential_cache_dir}"
chmod 700 "${krb5_dir}"
chmod 700 "${credential_cache_dir}"

# Build a self-contained Kerberos realm so the test does not depend on any host KDC,
# realm configuration, or preexisting tickets.
read -r kdc_port kadmin_port < <(find_free_tcp_ports 2)

cat <<EOF > "${krb5_config}"
[libdefaults]
	default_realm = ${realm}
	dns_lookup_kdc = false
	dns_lookup_realm = false
	dns_canonicalize_hostname = false
	qualify_shortname = ""
	rdns = false
	udp_preference_limit = 1

[realms]
	${realm} = {
		kdc = 127.0.0.1:${kdc_port}
		admin_server = 127.0.0.1:${kadmin_port}
	}

[domain_realm]
	localhost = ${realm}
	localhost. = ${realm}
EOF

cat <<EOF > "${kdc_config}"
[kdcdefaults]
	kdc_ports = ${kdc_port}
	kdc_tcp_ports = ${kdc_port}

[realms]
	${realm} = {
		database_name = ${krb5_dir}/principal
		admin_keytab = ${krb5_dir}/kadm5.keytab
		acl_file = ${krb5_dir}/kadm5.acl
		key_stash_file = ${krb5_dir}/stash
		kdc_ports = ${kdc_port}
		kdc_tcp_ports = ${kdc_port}
		max_life = 1h
		max_renewable_life = 2h
		supported_enctypes = aes256-cts:normal aes128-cts:normal
	}
EOF

export KRB5_CONFIG="${krb5_config}"
export KRB5_KDC_PROFILE="${kdc_config}"

# Ganesha needs an nfs/localhost service principal and keytab.
# The client side needs both user credentials for the mount and a host keytab for rpc.gssd.
"${kdb5_util_bin}" create -s -P "${master_password}" -r "${realm}"
"${kadmin_local_bin}" -q "addprinc -pw ${client_password} ${client_principal}"
"${kadmin_local_bin}" -q "addprinc -randkey nfs/localhost"
"${kadmin_local_bin}" -q "ktadd -k ${server_keytab} nfs/localhost"
chmod 600 "${server_keytab}"

"${krb5kdc_bin}" -n -P "${krb5_dir}/krb5kdc.pid" -r "${realm}" > "${krb5_dir}/krb5kdc.log" 2>&1 &
kdc_pid=$!

# Wait until the local KDC is listening before requesting tickets from it.
retry_command_with_attempts 10 nc -z 127.0.0.1 "${kdc_port}" || test_fail "KDC failed to start on port ${kdc_port}"

# KRB5CCNAME tells Kerberos tools which credential cache file to read or write.
printf '%s\n' "${client_password}" \
	| KRB5CCNAME="FILE:${credential_cache}" kinit "${client_principal}"
KRB5CCNAME="FILE:${credential_cache}" klist

printf '%s\n' "${client_password}" \
	| KRB5_CONFIG="${krb5_config}" KRB5CCNAME="FILE:${mount_credential_cache}" \
	sudo --preserve-env=KRB5_CONFIG,KRB5CCNAME kinit "${client_principal}"

KRB5_CONFIG="${krb5_config}" KRB5CCNAME="FILE:${mount_credential_cache}" \
	sudo --preserve-env=KRB5_CONFIG,KRB5CCNAME klist

# rpc_pipefs is the kernel/userspace handoff point for Linux NFS Kerberos upcalls.
# rpc.gssd listens there and turns kernel requests into Kerberos work.
sudo mount -t rpc_pipefs rpc_pipefs "${rpc_pipefs}"
mounted_rpc_pipefs=YES

# rpc.gssd needs host credentials for the client machine so it can answer the
# kernel's RPCSEC_GSS upcalls on behalf of the local NFS client.
client_keytab="${krb5_dir}/client.keytab"

client_hostname=$(hostname -f 2>/dev/null || hostname)
client_hostname=${client_hostname,,}

"${kadmin_local_bin}" -q "addprinc -randkey host/${client_hostname}"
"${kadmin_local_bin}" -q "ktadd -k ${client_keytab} host/${client_hostname}"
chmod 600 "${client_keytab}"

KRB5_CONFIG="${krb5_config}" sudo --preserve-env=KRB5_CONFIG \
	"${rpc_gssd_bin}" -f -vvv -r -R "${realm}" -p "${rpc_pipefs}" \
	-d "${credential_cache_dir}" \
	-k "${client_keytab}" \
	-n \
	> "${rpc_gssd_log}" 2>&1 &

assert_eventually "pgrep -f 'rpc.gssd.*${rpc_pipefs}' >/dev/null" "10 seconds"

create_ganesha_pid_file

# Ganesha enforces SecType and validates RPCSEC_GSS tokens before requests reach
# the SaunaFS FSAL. The FSAL itself is not doing Kerberos negotiation here.
cat <<EOF > "${ganesha_config}"
LOG {
	Default_Log_Level = INFO;
	Facility {
		name = FILE;
		destination = "${ganesha_log}";
		enable = active;
	}
}
NFS_KRB5 {
	PrincipalName = "nfs@localhost";
	KeytabPath = "${server_keytab}";
	CCacheDir = "${credential_cache_dir}";
	Active_krb5 = true;
}
NFSV4 {
	Grace_Period = 5;
	Lease_Lifetime = 5;
	# This test uses local account lookup to keep Kerberos principal-to-UID/GID
	# resolution self-contained via getpwnam.
	# It is not trying to validate every possible NFSv4 identity-mapping deployment.
	UseGetpwnam = true;
}
EXPORT
{
	Attr_Expiration_Time = 0;
	Export_Id = 99;
	Path = /;
	Pseudo = /;
	Access_Type = RW;
	Transports = TCP;
	SecType = krb5, krb5i, krb5p;
	Squash = No_Root_Squash;
	FSAL {
		Name = SaunaFS;
		hostname = localhost;
		port = ${saunafs_info_[matocl]};
		io_retries = 5;
		cache_expiration_time_ms = 2500;
	}
	Protocols = 4;
	CLIENT {
		Clients = localhost;
	}
}
EOF

KRB5_CONFIG="${krb5_config}" sudo --preserve-env=KRB5_CONFIG \
	/usr/bin/ganesha.nfsd -f "${ganesha_config}" -L "${ganesha_log}"
assert_eventually "grep -q 'nfs_krb5 functionality is now enabled' '${ganesha_log}'" \
	"15 seconds"

check_rpc_service

# The three Kerberos-backed security flavors should succeed for both NFSv4.1 and NFSv4.2.
for nfs_version in 4.1 4.2; do
	minor_version=${nfs_version#4.}

	# sec=sys is the negative control: the export should refuse AUTH_SYS.
	assert_failure sudo mount -t nfs4 \
		-o "nfsvers=${nfs_version},proto=tcp,sec=sys" \
		localhost:/ "${mountpoint_path}"

	# Exports can allow krb5, krb5i, and krb5p:
	# - krb5 provides authentication
	# - krb5i adds integrity protection
	# - krb5p adds privacy/encryption for NFS RPC traffic
	# Validate that Ganesha accepts each flavor and that basic I/O works under it.
	# The test checks integration behavior, not packet-level RPCSEC_GSS details.
	for security_flavor in krb5 krb5i krb5p; do
		if ! sudo mount -vvvv -t nfs4 \
			-o "nfsvers=${nfs_version},proto=tcp,sec=${security_flavor}" \
			localhost:/ "${mountpoint_path}"; then
			cat "${rpc_gssd_log}" || true
			cat "${ganesha_log}" || true
			test_fail "Failed to mount SaunaFS through Ganesha with NFSv${nfs_version} sec=${security_flavor}"
		fi

		test_file="${mountpoint_path}/kerberos-nfsv${nfs_version/./}-${security_flavor}.txt"
		echo "ganesha-nfsv${nfs_version}-${security_flavor}" > "${test_file}"
		assert_equals "ganesha-nfsv${nfs_version}-${security_flavor}" "$(cat "${test_file}")"

		mountstats_block=$(get_mountstats_block "${mountpoint_path}")
		if [[ -z ${mountstats_block} ]]; then
			test_fail "Could not find mountstats for ${mountpoint_path}"
		fi

		assert_mountstats_option "${mountstats_block}" "sec" "${security_flavor}"

		if ! grep -Eq "(^|,)vers=4[.]${minor_version}(,|$)|(^|,)minorversion=${minor_version}(,|$)" \
				<<< "${mountstats_block}"; then
			test_fail "Expected an explicit NFSv${nfs_version} mount in mountstats:${mountstats_block}"
		fi

		sudo umount "${mountpoint_path}"
	done
done

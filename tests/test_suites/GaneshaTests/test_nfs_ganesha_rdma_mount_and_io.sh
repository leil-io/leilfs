# Verify the SaunaFS FSAL can serve NFSv4 over RPC-over-RDMA (NFS/RDMA).
#
# RDMA is a transport-layer feature in Ganesha/libntirpc, independent of the FSAL,
# so a successful RDMA mount of a SaunaFS export proves the FSAL works over RDMA.
#
# The test SKIPS (passes) cleanly when any prerequisite is missing, so it is safe
# in a pipeline whose Ganesha is built without RDMA. To actually exercise RDMA:
#   - Ganesha built with -DUSE_NFS_RDMA=ON (needs librdmacm/libibverbs).
#   - A soft-RDMA device: SoftiWARP (siw) or SoftRoCE (rdma_rxe).
#   - The rpcrdma client module, and (for siw) the iWARP port mapper: sudo iwpmd
#
# The scoped sudoers grants this test needs are provisioned by
# tests/setup_machine.sh (the "# Ganesha" and "# Ganesha RDMA" blocks) — see
# there for the exact, up-to-date command list.

timeout_set 1 minute

readonly rdma_dev="sfs_rdma0"
readonly rdma_port=20049 # Ganesha default NFS_RDMA_Port

# --- Skip unless the RDMA prerequisites are present -------------------------
ganesha_bin=$(command -v ganesha.nfsd || echo /usr/bin/ganesha.nfsd)
if ! ldd "${ganesha_bin}" 2>/dev/null | grep -qE 'librdmacm|libibverbs'; then
	echo "SKIP: Ganesha not built with NFS/RDMA (-DUSE_NFS_RDMA=ON)."
	test_end
fi
if ! is_program_installed rdma || ! is_program_installed ibv_devinfo; then
	echo "SKIP: iproute2 'rdma' or ibverbs-utils 'ibv_devinfo' not installed."
	test_end
fi

# Bind the soft-RDMA device to a real NIC (the default-route interface) and mount
# via its routable IP. A loopback device (siw/rxe on 'lo', 127.0.0.1) brings the
# port up but does not deliver the RPC data path for a same-host mount; a real
# netdev gives the device a routable GID that the client can connect to. With no
# routable interface there is nothing usable to bind, so skip rather than fall
# back to a loopback mount that cannot carry the RDMA data path.
# Read the interface as the token after 'dev' so a gateway-less default route
# ("default dev eth0 ...", e.g. point-to-point links) parses correctly too.
rdma_netdev=$(ip route show default 2>/dev/null | awk '{for(i=1;i<=NF;i++) if($i=="dev"){print $(i+1); exit}}')
server_ip=$(ip -o -4 addr show "${rdma_netdev}" 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -1)
if [[ -z ${rdma_netdev} || -z ${server_ip} ]]; then
	echo "SKIP: no routable IPv4 interface for the RDMA data path."
	test_end
fi

setup_soft_rdma() {
	# Drop any leftover device from a crashed run; otherwise "rdma link add"
	# fails with "File exists" and the test misreads it as no kernel support.
	# Expected to fail on a clean run (no such device), so ignore the result.
	sudo rdma link delete "${rdma_dev}" 2>/dev/null || true
	sudo modprobe siw 2>/dev/null &&
		sudo rdma link add "${rdma_dev}" type siw netdev "${rdma_netdev}" 2>/dev/null && return 0
	sudo modprobe rdma_rxe 2>/dev/null &&
		sudo rdma link add "${rdma_dev}" type rxe netdev "${rdma_netdev}" 2>/dev/null && return 0
	return 1
}
if ! setup_soft_rdma; then
	echo "SKIP: kernel has no SoftiWARP (siw) or SoftRoCE (rdma_rxe) support."
	test_end
fi

# Single EXIT trap, registered as soon as the RDMA link exists. Defining cleanup
# here (not after the mount is up) guarantees the link and any iwpmd we start are
# torn down even if setup fails in between. Registering a second EXIT trap later
# would silently replace this one and leak those resources.
started_iwpmd=0
test_error_cleanup() {
	set +e
	if [[ -n ${mountpoint_path:-} ]] && mountpoint -q "${mountpoint_path}"; then
		sudo umount -l "${mountpoint_path}"
	fi
	sudo pkill -9 ganesha.nfsd 2>/dev/null
	sudo rdma link delete "${rdma_dev}" 2>/dev/null
	if [[ ${started_iwpmd} == 1 ]]; then
		sudo pkill -x iwpmd 2>/dev/null
	fi
}
trap test_error_cleanup EXIT
echo "RDMA: device ${rdma_dev} on ${rdma_netdev}, mounting via ${server_ip}"

# siw is iWARP; its connection setup needs the iWARP port mapper (iwpmd) running.
# systemd keeps iwpmd stopped (StopWhenUnneeded=yes) without RDMA hardware, so
# start it standalone when a siw device needs it. Skip only if it cannot start.
# Record whether WE started it (started_iwpmd, initialized above), so cleanup
# only stops a daemon we launched.
if ibv_devinfo -d "${rdma_dev}" 2>/dev/null | grep -qi iWARP && ! pgrep -x iwpmd >/dev/null 2>&1; then
	sudo iwpmd 2>/dev/null || true
	sleep 1
	if ! pgrep -x iwpmd >/dev/null 2>&1; then
		echo "SKIP: could not start the iWARP port mapper (iwpmd)."
		test_end
	fi
	started_iwpmd=1
fi

# NFS-over-RDMA client transport. Required by the mount below; without it the
# client can't speak RPC/RDMA and "mount -o rdma" would hard-fail or hang. Gate
# on the module being present (loaded or builtin) rather than modprobe's exit
# code, so a builtin rpcrdma (CONFIG_SUNRPC_XPRT_RDMA=y) is not misread as absent.
sudo modprobe rpcrdma 2>/dev/null || true
if [[ ! -d /sys/module/rpcrdma ]]; then
	echo "SKIP: kernel has no NFS-over-RDMA client transport (rpcrdma)."
	test_end
fi

# --- SaunaFS + Ganesha ------------------------------------------------------
CHUNKSERVERS=3 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

mountpoint_path="${TEMP_DIR}/mnt/ganesha-rdma"
ganesha_config="${info[mount0]}/ganesha.conf"
ganesha_log="${TEMP_DIR}/ganesha-rdma.log"

mkdir -p "${mountpoint_path}"
create_ganesha_pid_file

# RDMA is enabled by the "nfsrdma" token in NFS_Protocols plus the RDMA transport
# on the export. TCP stays enabled for the NFSv4 negotiation path.
cat <<EOF >"${ganesha_config}"
NFS_CORE_PARAM {
	NFS_Protocols = 4, nfsrdma;
	NFS_RDMA_Port = ${rdma_port};
	NFS_RDMA_Protocol_Versions = 4.0, 4.1, 4.2;
}
NFSV4 {
	Grace_Period = 5;
	Lease_Lifetime = 5;
}
EXPORT {
	Export_Id = 77;
	Path = /;
	Pseudo = /;
	Access_Type = RW;
	Transports = TCP, RDMA;
	Protocols = 4;
	FSAL {
		Name = SaunaFS;
		hostname = localhost;
		port = ${saunafs_info_[matocl]};
	}
	CLIENT {
		Clients = ${server_ip}, localhost;
	}
}
EOF

sudo /usr/bin/ganesha.nfsd -f "${ganesha_config}" -L "${ganesha_log}"

# Wait for the NFSv4 control port. showmount/check_rpc_service speak the v3 MOUNT
# protocol, which an NFSv4-only export does not register.
wait_for_tcp_port() {
	local port=$1 tries=30
	while ((tries-- > 0)); do
		if ss -ltn 2>/dev/null | grep -qE "[:.]${port}([[:space:]]|$)"; then
			return 0
		fi
		sleep 1
	done
	return 1
}
if ! wait_for_tcp_port 2049; then
	cat "${ganesha_log}"
	test_fail "Ganesha did not open the NFSv4 control port 2049"
fi
if ! grep -qi 'LISTENING for RPC/RDMA' "${ganesha_log}"; then
	cat "${ganesha_log}"
	test_fail "Ganesha did not bring up the RDMA listener"
fi

# --- Mount over RDMA (the asserted result) ----------------------------------
sudo mount -vvvv -t nfs -o "rdma,port=${rdma_port},sec=sys" "${server_ip}:/" "${mountpoint_path}"

mount_opts=$(findmnt -no OPTIONS --target "${mountpoint_path}")
if [[ ${mount_opts} != *rdma* ]]; then
	test_fail "Mount is not using the RDMA transport: ${mount_opts}"
fi
assert_awk_finds_no '/xprt:\ttcp/' \
	"$(awk -v m="${mountpoint_path}" \
		'$1=="device" && index($0," mounted on "m" "){f=1} f{print} f&&$0==""{exit}' \
		/proc/self/mountstats)"

# Data path over RDMA is working if we can read/write files.
# Write a test file, then read it back and validate the contents.
test_file="${mountpoint_path}/rdma-roundtrip.bin"
FILE_SIZE=4194304 file-generate "${test_file}"
file-validate "${test_file}"

sudo umount "${mountpoint_path}"
test_error_cleanup
trap - EXIT

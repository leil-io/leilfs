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
# The GaneshaTests suite launches the binary installed at /usr/bin/ganesha.nfsd;
# use that same path for the RDMA-support check so we never validate one build
# and run another (a PATH-resolved /usr/local/bin binary could differ).
ganesha_bin=/usr/bin/ganesha.nfsd
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

# Cleanup and the single EXIT trap are defined before any RDMA device or daemon
# is created, so a failure at any point tears them down. Registering a second
# EXIT trap later would silently replace this one and leak those resources.
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

# siw is iWARP; its connection setup needs the iWARP port mapper (iwpmd) running.
# systemd keeps iwpmd stopped (StopWhenUnneeded=yes) without RDMA hardware, so
# start it standalone. Record whether WE started it, so cleanup only stops a
# daemon we launched. Returns non-zero when iwpmd is unavailable.
ensure_iwpmd() {
	pgrep -x iwpmd >/dev/null 2>&1 && return 0
	is_program_installed iwpmd || return 1
	sudo iwpmd 2>/dev/null || true
	sleep 1
	pgrep -x iwpmd >/dev/null 2>&1 || return 1
	started_iwpmd=1
}

# Bring up a soft-RDMA device on the chosen netdev. An absent module is a clean
# skip; a module that is present but cannot create the device is a real failure
# (bad netdev, sudoers/permission regression) surfaced with its stderr rather
# than hidden behind a "no kernel support" skip. Prefer siw, but fall back to
# SoftRoCE (rdma_rxe, needs no daemon) when the iWARP port mapper is missing.
setup_soft_rdma() {
	# Drop any leftover device from a crashed run so "rdma link add" does not fail
	# with "File exists"; expected to fail on a clean run, so ignore the result.
	sudo rdma link delete "${rdma_dev}" 2>/dev/null || true

	local err siw_present=0
	if modinfo siw >/dev/null 2>&1 && sudo modprobe siw 2>/dev/null; then
		siw_present=1
		if err=$(sudo rdma link add "${rdma_dev}" type siw netdev "${rdma_netdev}" 2>&1); then
			if ensure_iwpmd; then
				return 0
			fi
			echo "NOTE: siw is up but the iWARP port mapper (iwpmd) is unavailable; trying SoftRoCE."
			sudo rdma link delete "${rdma_dev}" 2>/dev/null || true
		else
			test_fail "rdma link add (siw) failed though the siw module is present: ${err}"
		fi
	fi

	if modinfo rdma_rxe >/dev/null 2>&1 && sudo modprobe rdma_rxe 2>/dev/null; then
		if err=$(sudo rdma link add "${rdma_dev}" type rxe netdev "${rdma_netdev}" 2>&1); then
			return 0
		fi
		test_fail "rdma link add (rxe) failed though the rdma_rxe module is present: ${err}"
	fi

	if ((siw_present)); then
		echo "SKIP: SoftiWARP (siw) present but iwpmd unavailable, and no SoftRoCE (rdma_rxe)."
	else
		echo "SKIP: kernel has no SoftiWARP (siw) or SoftRoCE (rdma_rxe) support."
	fi
	return 1
}
if ! setup_soft_rdma; then
	test_end
fi
echo "RDMA: device ${rdma_dev} on ${rdma_netdev}, mounting via ${server_ip}"

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

sudo "${ganesha_bin}" -f "${ganesha_config}" -L "${ganesha_log}"

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

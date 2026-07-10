#!/usr/bin/env bash
set -eux -o pipefail
readonly self="$(readlink -f "${BASH_SOURCE[0]}")"
readonly script_dir="$(dirname "$self")"
readonly script_name="$(basename "${self}")"

usage() {
	cat >&2 <<-EOF
		Usage: ${script_name} setup hdd...

		This scripts prepares the machine to run SaunaFS tests here.
		Specifically:
		* creates users saunafstest, saunafstest_0, ..., saunafstest_9
		* adds all saunafstest users to the fuse group
		* grants all users rights to run programs as saunafstest users
		* grants all users rights to run 'pkill -9 -u <some saunafstest user>'
		* allows all users to mount fuse filesystem with allow_other option
		* creates a 2G ramdisk in /mnt/ramdisk
		* creates 6 files mounted using loop device

		Example:
		${script_name} setup /mnt
		${script_name} setup /mnt/hda /mnt/hdb

		You need root permissions to run this script
	EOF
	exit 1
}

tool_exists() {
	type -P -v "${1}" >/dev/null
}

minimum_number_of_args=2
if [[ ( ( "${1}" != "setup" ) && ( "${1}" != "setup-force" ) ) || ( ${#} -lt ${minimum_number_of_args} ) ]]; then
	usage >&2
fi

if grep -q 'saunafstest_loop' /etc/fstab; then
	if [[ "${1}" != "setup-force" ]]; then
		echo 'The machine is at least partially configured'
		echo 'Run revert-setup-machine.sh to revert the current configuration'
		exit 1
	fi
fi

shift
umask 0022

"$script_dir/install-packages.sh"

python_packages=(
	black
	devscripts
	flask
	lcov_cobertura
	mypy
	pdfminer.six
	requests
	types-requests
)

gpg2 --list-keys
#setup gtest from sources
: "${GTEST_ROOT:=/usr/local}"
readonly gtest_temp_build_dir="$(mktemp -d)"
cmake -S /usr/src/googletest -B "${gtest_temp_build_dir}" -DCMAKE_INSTALL_PREFIX="${GTEST_ROOT}"
make -C "${gtest_temp_build_dir}" install
rm -rf "${gtest_temp_build_dir:?}"
# Setup python3 with a global virtual env (new recommended way)
VIRTUAL_ENV=/var/lib/saunafs_setup_machine_venv
export VIRTUAL_ENV
if ! grep -Eq 'PATH=.*'"${VIRTUAL_ENV}/bin\b"'' /etc/environment ; then
	# Add the virtualenv to the end of the PATH in /etc/environment
	# Select three groups: PATH=, optional quote, rest of the path,
	# then add the virtualenv bin directory to the end of the path, keeping the correct quoting
	sed -E 's@(PATH=)([:"'\'']?)(.*)\2@\1\2\3:'"${VIRTUAL_ENV}/bin"'\2@' -i /etc/environment;
fi
# Add VIRTUAL_ENV to /etc/environment if it's not there
sed '\@VIRTUAL_ENV="'"${VIRTUAL_ENV}"'"@!s@$@\nVIRTUAL_ENV="'"${VIRTUAL_ENV}"'"@' -zi /etc/environment
python3 -m venv "${VIRTUAL_ENV}"
"${VIRTUAL_ENV}/bin/python3" -m pip install "${python_packages[@]}"

echo ; echo 'Add group fuse'
groupadd -f fuse

echo ; echo 'Add user saunafstest'
if ! getent passwd saunafstest > /dev/null; then
	useradd --system --user-group --home /var/lib/saunafstest --create-home saunafstest
	chmod 755 /var/lib/saunafstest
fi

if ! groups saunafstest | grep -w fuse > /dev/null; then
	usermod -a -G fuse saunafstest # allow this user to mount fuse filesystem
fi
if ! groups saunafstest | grep -w adm > /dev/null; then
	usermod -a -G adm saunafstest # allow this user to read files from /var/log
fi

echo ; echo 'Prepare sudo configuration'
if ! [[ -d /etc/sudoers.d ]]; then
	# Sudo is not installed by default on Debian machines
	echo "sudo not installed!" >&2
	echo "Install it manually: apt-get install sudo" >&2
	echo "Then run this script again" >&2
	exit 1
fi
if ! [[ -f /etc/sudoers.d/saunafstest ]] || \
		! grep drop_caches /etc/sudoers.d/saunafstest >/dev/null; then
	cat >/etc/sudoers.d/saunafstest <<-END
		Defaults rlimit_core=default
		ALL ALL = (saunafstest) NOPASSWD: ALL
		ALL ALL = NOPASSWD: /usr/bin/pkill -9 -u saunafstest
		ALL ALL = NOPASSWD: /bin/rm -rf /tmp/saunafs_error_dir
		saunafstest ALL = NOPASSWD: /bin/sh -c echo\ 1\ >\ /proc/sys/vm/drop_caches
		saunafstest ALL = NOPASSWD: /usr/bin/cat .oplog
		saunafstest ALL = NOPASSWD: /usr/bin/tee .stats
	END
	chmod 0440 /etc/sudoers.d/saunafstest
fi
if ! [[ -d /etc/security/limits.d ]]; then
	echo "pam module pam_limits is not installed!" >&2
	exit 1
fi
if ! [[ -f /etc/security/limits.d/10-saunafstests.conf ]]; then
	# Change limit of open files for user saunafstest
	echo "saunafstest hard nofile 10000" > /etc/security/limits.d/10-saunafstests.conf
	chmod 0644 /etc/security/limits.d/10-saunafstests.conf
fi
if ! grep 'pam_limits.so' /etc/pam.d/sudo > /dev/null; then
	cat >>/etc/pam.d/sudo <<-END
		### Reload pam limits on sudo - necessary for saunafs tests. ###
		session required pam_limits.so
	END
fi

echo ; echo 'Add users saunafstest_{0..9}'
for username in saunafstest_{0..9}; do
	if ! getent passwd ${username} > /dev/null; then
		useradd --system --user-group --home /var/lib/${username} --create-home \
				--groups fuse,saunafstest ${username}
		cat >>/etc/sudoers.d/saunafstest <<-END

			ALL ALL = (${username}) NOPASSWD: ALL
			ALL ALL = NOPASSWD: /usr/bin/pkill -9 -u ${username}
		END
	fi
done

if [ ! -f /etc/sudoers.d/saunafstest ] || ! grep -q '# SMR' /etc/sudoers.d/saunafstest >/dev/null; then
	cat <<-'END' >>/etc/sudoers.d/saunafstest
		# SMR
		saunafstest ALL = NOPASSWD: /usr/sbin/modprobe null_blk nr_devices=0
		saunafstest ALL = NOPASSWD: /usr/sbin/mkzonefs -f -o perm=666 /dev/sauna_nullb*
		saunafstest ALL = NOPASSWD: /usr/sbin/mkzonefs -f -o perm=666 /dev/nullb*
		saunafstest ALL = NOPASSWD: /usr/bin/mount -t zonefs /dev/sauna_nullb* /mnt/zoned/sauna_nullb*
		saunafstest ALL = NOPASSWD: /usr/bin/mount -t zonefs /dev/nullb* /mnt/zoned/sauna_nullb*
		saunafstest ALL = NOPASSWD: /usr/bin/umount -l /mnt/zoned/sauna_nullb*

		saunafstest ALL = NOPASSWD: /usr/bin/mkdir -pm 777 /mnt/zoned/sauna_nullb*
		saunafstest ALL = NOPASSWD: /usr/bin/mkdir /sys/kernel/config/nullb/sauna_nullb*
		saunafstest ALL = NOPASSWD: /usr/bin/rmdir /sys/kernel/config/nullb/sauna_nullb*
		saunafstest ALL = NOPASSWD: /usr/bin/tee /sys/kernel/config/nullb/sauna_nullb*
		saunafstest ALL = NOPASSWD: /usr/bin/tee /sys/block/sauna_nullb*
		saunafstest ALL = NOPASSWD: /usr/bin/tee /sys/block/nullb*
	END
fi

if [ ! -f /etc/sudoers.d/saunafstest ] || ! grep -q '# Ganesha' /etc/sudoers.d/saunafstest >/dev/null; then
	cat <<-'END' >>/etc/sudoers.d/saunafstest
		# Ganesha automated tests
		saunafstest ALL = NOPASSWD: /tmp/SaunaFS-autotests/mnt/sfs0/bin/ganesha.nfsd
		saunafstest ALL = NOPASSWD: /usr/bin/ganesha.nfsd
		saunafstest ALL = NOPASSWD: /usr/bin/pkill -9 ganesha.nfsd
		saunafstest ALL = NOPASSWD: /usr/bin/pkill -HUP rpcbind
		saunafstest ALL = NOPASSWD: /usr/bin/mkdir -p /var/run/ganesha
		saunafstest ALL = NOPASSWD: /usr/bin/touch /var/run/ganesha/ganesha.pid
		saunafstest ALL = NOPASSWD: /usr/bin/mount, /usr/bin/umount
	END
fi

if [ ! -f /etc/sudoers.d/saunafstest ] || ! grep -q '# Ganesha RDMA' /etc/sudoers.d/saunafstest >/dev/null; then
	cat <<-'END' >>/etc/sudoers.d/saunafstest
		# Ganesha RDMA (NFS over RDMA) automated test
		saunafstest ALL = NOPASSWD: /usr/sbin/modprobe siw
		saunafstest ALL = NOPASSWD: /usr/sbin/modprobe rdma_rxe
		saunafstest ALL = NOPASSWD: /usr/sbin/modprobe rpcrdma
		saunafstest ALL = NOPASSWD: /usr/bin/rdma link add sfs_rdma0 type siw netdev *
		saunafstest ALL = NOPASSWD: /usr/bin/rdma link add sfs_rdma0 type rxe netdev *
		saunafstest ALL = NOPASSWD: /usr/bin/rdma link delete sfs_rdma0
		saunafstest ALL = NOPASSWD: /usr/sbin/iwpmd
		saunafstest ALL = NOPASSWD: /usr/bin/pkill -x iwpmd
	END
fi

if [ ! -f /etc/sudoers.d/saunafstest ] || ! grep -q '# Kerberos' /etc/sudoers.d/saunafstest >/dev/null; then
	cat <<-'END' >>/etc/sudoers.d/saunafstest
		# Kerberos automated tests
		Defaults:saunafstest env_keep += "KRB5_CONFIG KRB5CCNAME"
		saunafstest ALL = NOPASSWD: /usr/bin/kinit saunafstest
		saunafstest ALL = NOPASSWD: /usr/bin/klist
		saunafstest ALL = NOPASSWD: /usr/bin/kdestroy
		saunafstest ALL = NOPASSWD: /usr/sbin/rpc.gssd -f -vvv -r -R SAUNAFS.TEST \
			-p /tmp/SaunaFS-autotests/* -d /tmp/SaunaFS-autotests/* -k /tmp/SaunaFS-autotests/* -n
		saunafstest ALL = NOPASSWD: /sbin/rpc.gssd -f -vvv -r -R SAUNAFS.TEST \
			-p /tmp/SaunaFS-autotests/* -d /tmp/SaunaFS-autotests/* -k /tmp/SaunaFS-autotests/* -n
		saunafstest ALL = NOPASSWD: /usr/bin/pkill -f rpc.gssd*
	END
fi

if [ ! -f /etc/sudoers.d/saunafstest ] || ! grep -q '# Client' /etc/sudoers.d/saunafstest >/dev/null; then
	cat <<-'END' >>/etc/sudoers.d/saunafstest
		# Client
		saunafstest ALL = NOPASSWD: /usr/bin/tee /tmp/SaunaFS-autotests/mnt/sfs*
	END
fi

if [ ! -f /etc/sudoers.d/saunafstest ] || ! grep -q '# FoundationDB' /etc/sudoers.d/saunafstest > /dev/null; then
	cat <<-'END' >>/etc/sudoers.d/saunafstest
		# FoundationDB
		saunafstest ALL = NOPASSWD: /usr/lib/foundationdb/fdbmonitor --conffile*
		saunafstest ALL = NOPASSWD: /usr/bin/pkill -f fdbmonitor*
		saunafstest ALL = NOPASSWD: /usr/bin/pkill -9 -f fdbmonitor*
	END
fi

echo ; echo 'Install FoundationDB'
if grep -q Microsoft /proc/version && ! grep -q microsoft-standard /proc/version; then
	echo "Running on WSL1: skipping FoundationDB installation."
else
	"$script_dir/ci_build/install-foundationdb.sh"
fi

echo ; echo 'Fixing GIDs of users'
for name in saunafstest saunafstest_{0..9}; do
	uid=$(getent passwd "${name}" | cut -d: -f3)
	gid=$(getent group  "${name}" | cut -d: -f3)
	if [[ ${uid} == "" || ${gid} == "" ]]; then
		exit 1
	fi
	if [[ "${uid}" == "${gid}" ]]; then
		# UID is equal to GID -- we have to change it to something different,
		# so find some other free gid
		new_gid=$((gid * 2))
		while getent group ${new_gid}; do
			new_gid=$((new_gid + 1))
		done
		groupmod -g ${new_gid} ${name}
	fi
done

echo ; echo 'Prepare /etc/fuse.conf'
if ! grep '^[[:blank:]]*user_allow_other' /etc/fuse.conf >/dev/null; then
	echo "user_allow_other" >> /etc/fuse.conf
fi

echo ; echo 'Configure local port range for outbond connections'
if [ ! -f  /etc/sysctl.d/60-ip_port_range.conf ]; then
	echo "net.ipv4.ip_local_port_range=10000 64000" > /etc/sysctl.d/60-ip_port_range.conf
fi

echo ; echo 'Prepare empty /etc/saunafs_tests.conf'
if ! [[ -f /etc/saunafs_tests.conf ]]; then
	cat >/etc/saunafs_tests.conf <<-END
		: \${SAUNAFS_DISKS:="$*"}
		# : \${TEMP_DIR:=/tmp/SaunaFS-autotests}
		# : \${SAUNAFS_ROOT:=${HOME}/local}
		# : \${FIRST_PORT_TO_USE:=25000}
	END
fi

echo ; echo 'Prepare ramdisk'
if ! grep /mnt/ramdisk /etc/fstab >/dev/null; then
	echo "# Ramdisk used in SaunaFS tests" >> /etc/fstab
	echo "ramdisk  /mnt/ramdisk  tmpfs  mode=1777,size=2g" >> /etc/fstab
	mkdir -p /mnt/ramdisk
	mount /mnt/ramdisk
	# shellcheck disable=SC2016
	echo ': ${RAMDISK_DIR:=/mnt/ramdisk}' >> /etc/saunafs_tests.conf
fi

if grep -q Microsoft /proc/version && ! grep -q microsoft-standard /proc/version; then
	echo "Running on WSL1: skipping loop device creation."
else
	echo ; echo 'Prepare loop devices'
	#creating loop devices more or less evenly distributed between available disks
	i=0
	devices=6
	loops=()
	while [ ${i} -lt ${devices} ] ; do
		for disk in "$@"; do
			if (( i == devices )); then #stop if we have enough devices
				break
			fi
			loops+=("/mnt/saunafstest_loop_${i}")
			if grep -q "saunafstest_loop_${i}" /etc/fstab; then
				(( ++i ))
				continue
			fi
			mkdir -p "${disk}/saunafstest_images"
			# Create image file
			image="${disk}/saunafstest_images/image_${i}"
			truncate -s 1G "${image}"
			mkfs.ext4 -Fq "${image}"
			# Add it to fstab
			echo "$(readlink -m "${image}") /mnt/saunafstest_loop_${i}  ext4  loop" >> /etc/fstab
			mkdir -p "/mnt/saunafstest_loop_${i}"
			# Mount and set permissions
			mount "/mnt/saunafstest_loop_${i}"
			chmod 1777 "/mnt/saunafstest_loop_${i}"
			(( ++i ))
		done
	done
	# shellcheck disable=SC2016
	echo ': ${SAUNAFS_LOOP_DISKS:="'"${loops[*]}"'"}' >> /etc/saunafs_tests.conf
fi

set +x
echo 'Machine configured successfully'

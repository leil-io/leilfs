#!/bin/bash
set -x

readonly self="$(readlink -f "${BASH_SOURCE[0]}")"
readonly script_dir="$(dirname "$self")"

export DEBIAN_FRONTEND="${DEBIAN_FRONTEND:-noninteractive}"

echo 'Installing necessary programs'
# lsb_release is required by both build scripts and this script -- install it first
if ! command -v  lsb_release >/dev/null; then
	if command -v dnf >/dev/null; then
		dnf -y install redhat-lsb-core
	elif command -v  yum >/dev/null; then
		yum -y install redhat-lsb-core
	elif command -v  apt-get >/dev/null; then
		apt-get update
		apt-get -y install lsb-release
	fi
fi

common_packages=(
	acl
	attr
	automake
	bash-completion
	bc
	ccache
	clang
	cmake
	curl
	dbench
	fakeroot
	fio
	fuse3
	gcc
	git
	gnupg2
	kmod
	lcov
	make
	nfs4-acl-tools
	pkg-config
	pylint
	python3-pip
	python3-setuptools
	python3-wheel
	psmisc
	pv
	rsync
	rsyslog
	ruby
	socat
	sudo
	tidy
	time
	valgrind
	wget
	bison
	flex
	tree
)
apt_packages=(
	debhelper
	devscripts
	build-essential
	iproute2
	libblkid-dev
	libboost-filesystem-dev
	libboost-iostreams-dev
	libboost-program-options-dev
	libboost-system-dev
	libcrcutil-dev
	libdb-dev
	libfmt-dev
	libfuse3-dev
	libgoogle-perftools-dev
	libgtest-dev
	libisal-dev
	libjudy-dev
	libpam0g-dev
	libspdlog-dev
	libssl-dev
	libsystemd-dev
	libthrift-dev
	libtirpc-dev
	liburcu-dev
	libyaml-cpp-dev
	netcat-openbsd
	python3-venv
	uuid-dev
	zlib1g-dev
	software-properties-common
	# NFS-Ganesha build + runtime deps live in
	# tests/ci_build/ganesha/ganesha-deps.txt, installed only by the Ganesha
	# test path so non-Ganesha machines skip them.
)
noble_packages=(
	prometheus-cpp-dev
	util-linux-extra
)
dnf_packages=(
	perl-IPC-Cmd # Required for vcpkg
	perl-Time-Piece # Required by vcpkg
	isa-l
	boost-filesystem
	boost-iostreams
	boost-program-options
	boost-system
	dnf-utils
	fmt-devel
	fuse3-devel
	gcc-c++
	gperftools-libs
	gtest-devel
	iproute
	iozone              # required by Ganesha -> iozone suite
	isa-l-devel
	iwpmd               # iWARP port mapper (siw); separate pkg on Fedora, not in rdma-core
	Judy-devel
	kernel-devel
	libblkid-devel
	libcutl-devel
	libdb-devel
	libibverbs-utils    # ibv_devices / ibv_devinfo (NFS/RDMA Ganesha test)
	libnsl
	libtirpc-devel
	netcat
	pam-devel
	pkgconfig
	python3-virtualenv
	rdma-core           # librdmacm / rdma tooling runtime (NFS/RDMA Ganesha test)
	rpm-build
	spdlog-devel
	systemd-devel
	thrift-devel
	userspace-rcu-devel
	uuid-devel
	yaml-cpp-devel
	zlib
	zlib-devel
	## For NFS-Ganesha tests (duplicate are commented out as reference)
	# gcc-c++
	# kernel-devel
	# userspace-rcu-devel
	dbus-devel
	docbook-dtds
	docbook-style-xsl
	jemalloc-devel
	keyutils
	krb5-libs
	krb5-server
	krb5-workstation
	libacl-devel
	libcap-devel
	libnfsidmap-devel
	libprometheus-cpp-devel
	libsqlite3x-devel
	nfs-utils
	xfsprogs-devel
)

# determine which OS we are running and choose the right set of packages to be installed
release="$(lsb_release -si)/$(lsb_release -sr)"
case "${release}" in
	Ubuntu/24.04)
		echo $release
		apt-get -y install ca-certificates-java # https://www.mail-archive.com/debian-bugs-dist@lists.debian.org/msg1911078.html
		apt-get -y install "${common_packages[@]}" "${apt_packages[@]}" "${noble_packages[@]}"
		;;
	LinuxMint/*|Ubuntu/*|Debian/*)
		apt-get -y install ca-certificates-java # https://www.mail-archive.com/debian-bugs-dist@lists.debian.org/msg1911078.html
		apt-get -y install "${common_packages[@]}" "${apt_packages[@]}"
		;;
	Fedora/*|Rocky/*)
		dnf -y install "${common_packages[@]}" "${dnf_packages[@]}"
		update-alternatives --remove-all nc
		update-alternatives --install /usr/bin/nc nc /usr/bin/netcat 1
		;;
	*)
		set +x
		echo "Installation of required packages SKIPPED, '${release}' isn't supported by this script"
		;;
esac

case "${release}" in
	LinuxMint/*|Ubuntu/*|Debian/*)
		if ! "$script_dir/llvm.sh" 19; then
			echo "Error: Failed to install Clang 19 using llvm.sh script."
			exit 1
		fi

		;;
	*)
		set +x
		echo "Installation of clang19 SKIPPED, only in apt systems clang19 is installed automatically"
		set -x
esac

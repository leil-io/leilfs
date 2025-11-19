#!/usr/bin/env bash

# This script installs FoundationDB on a Debian-based system.
# It downloads the client and server packages from the official GitHub repository,
# installs them, and verifies the installation.

set -eu -o pipefail

readonly workspace="/tmp/saunafs-foundationdb-tmp"

function die() {
	echo "Error: ${*}" >&2

	# Check if the workspace exists and remove it
	if [ -d "${workspace}" ]; then
		echo "Removing temporary workspace..."
		rm -rf "${workspace:?}"
	fi

	exit 1
}

# Ensure the script is run as root
if [ "$(id -u)" -ne 0 ]; then
	die "This script must be run as root. Please use sudo."
fi

# FDB_VERSION_AVX2="7.3.63"
FDB_VERSION_NO_AVX2="7.3.62"

fdb_version="${FDB_VERSION_NO_AVX2}"

# Use a version without AVX2 requirement if the CPU does not support it (like some Proxmox VMs)
# if ! grep -q -w avx2 /proc/cpuinfo; then
# 	fdb_version="${FDB_VERSION_NO_AVX2}"
# 	echo "AVX2 not detected. Falling back to FoundationDB version ${fdb_version}."
# fi

# Check if FoundationDB is already installed by using fdbcli
if command -v fdbcli &> /dev/null; then
	echo "FoundationDB is already installed. Version:"
	fdbcli --version
	exit 0
fi

# Check if wget is installed
if ! command -v wget &> /dev/null; then
	die "wget is not installed. Please install wget first."
fi

# Create the temporary workspace
mkdir -p "${workspace}"

# Define some useful variables
client_pkg="foundationdb-clients_${fdb_version}-1_amd64.deb"
server_pkg="foundationdb-server_${fdb_version}-1_amd64.deb"
hash_suffix=".sha256"
base_url="https://github.com/apple/foundationdb/releases/download/${fdb_version}"

# Download the client and server packages
echo "Downloading FoundationDB packages..."
wget "${base_url}/${client_pkg}" -O "${workspace}/${client_pkg}" || die "Failed to download client package"
wget "${base_url}/${server_pkg}" -O "${workspace}/${server_pkg}" || die "Failed to download server package"

# Download the SHA256 hash file
wget "${base_url}/${client_pkg}${hash_suffix}" -O "${workspace}/${client_pkg}${hash_suffix}" || die "Failed to download client hash file"
wget "${base_url}/${server_pkg}${hash_suffix}" -O "${workspace}/${server_pkg}${hash_suffix}" || die "Failed to download server hash file"

# Verify the downloaded packages
echo "Verifying downloaded packages..."
cd "${workspace}" || die "Failed to change directory to ${workspace}"
sha256sum -c "${client_pkg}${hash_suffix}" || die "Client package verification failed"
sha256sum -c "${server_pkg}${hash_suffix}" || die "Server package verification failed"
cd - || die "Failed to change back to previous directory"

# Install the packages
echo "Installing FoundationDB packages..."
dpkg -i "${workspace}/${client_pkg}" "${workspace}/${server_pkg}" || die "Failed to install FoundationDB packages"

# Fix any missing dependencies
echo "Fixing missing dependencies..."
apt-get install -f -y || die "Failed to fix missing dependencies"

# Verify the installation
echo "Verifying FoundationDB installation..."
if command -v fdbcli &> /dev/null; then
	echo "FoundationDB CLI installed successfully."
	fdbcli --version
else
	echo "FoundationDB CLI installation failed."
	exit 1
fi

# Clean up temporary workspace
echo "Removing temporary workspace..."
rm -r "${workspace:?}"

echo "FoundationDB installation completed successfully."

#!/bin/bash
set -e

# Ensure we are in utils/wireshark and that plugins/epan/saunafs exists
if [[ ! -d "plugins/epan/saunafs" ]]; then
	echo "❌ Error: This script must be run from the utils/wireshark directory."
	echo "Missing directory: plugins/epan/saunafs"
	exit 1
fi

# Define paths
SAUNAFS_SRCDIR=$(cd ../../ && pwd)
WIRESHARK_SRCDIR="/tmp/wireshark"
SFS_HEADER_PATH="${SAUNAFS_SRCDIR}/src/protocol/SFSCommunication.h"

echo "📂 Using SAUNAFS_SRCDIR: ${SAUNAFS_SRCDIR}"
echo "📂 Using WIRESHARK_SRCDIR: ${WIRESHARK_SRCDIR}"

# Install dependencies
echo "📦 Installing dependencies..."
sudo apt update
sudo apt install -y wireshark cmake libcap-dev libglib2.0-dev bison flex \
	libcrypto++-dev libc-ares-dev libgcrypt20 libgcrypt20-dev \
	libspeexdsp1 libspeexdsp-dev

# Get Wireshark version
WIRESHARK_VERSION=$(wireshark --version | grep -Eo '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
echo "🔍 Detected Wireshark version: ${WIRESHARK_VERSION}"

# Clone Wireshark sources if not already present
if [[ ! -d "${WIRESHARK_SRCDIR}" ]]; then
	echo "🔧 Cloning Wireshark sources into ${WIRESHARK_SRCDIR}..."
	git clone https://gitlab.com/wireshark/wireshark.git/ "${WIRESHARK_SRCDIR}"
	cd "${WIRESHARK_SRCDIR}"
	git checkout "wireshark-${WIRESHARK_VERSION}"
else
	echo "ℹ️ Wireshark source directory already exists. Skipping clone."
	cd "${WIRESHARK_SRCDIR}"
	git fetch
	git checkout "wireshark-${WIRESHARK_VERSION}"
fi

# Copy SaunaFS dissector files
echo "📁 Copying SaunaFS plugin files into Wireshark source tree..."
tar -C "${SAUNAFS_SRCDIR}/utils/wireshark" -c . | tar -C "${WIRESHARK_SRCDIR}" -vx

# Generate dissector code
echo "⚙️ Generating dissector code..."
"${WIRESHARK_SRCDIR}/plugins/epan/saunafs/generate.sh" "${SFS_HEADER_PATH}" "${WIRESHARK_VERSION}"

# Patch CMakeLists.txt if necessary
echo "🛠️ Patching CMakeLists.txt if needed..."
CMAKE_FILE="${WIRESHARK_SRCDIR}/CMakeLists.txt"
if ! grep -q "plugins/epan/saunafs" "${CMAKE_FILE}"; then
	sed -i '/plugins\/epan\/profinet/a\		plugins/epan/saunafs' "${CMAKE_FILE}"
fi

# Build Wireshark with plugin
echo "🏗️ Building Wireshark..."
mkdir -p "${WIRESHARK_SRCDIR}/build"
cd "${WIRESHARK_SRCDIR}/build"
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_wireshark=NO
nice make -j$(nproc)

# Install the plugin
PLUGIN_PATH="${WIRESHARK_SRCDIR}/build/run/saunafs.so"
PERSONAL_CONFIG_DIR="${HOME}/.local/lib/wireshark/plugins/${WIRESHARK_VERSION}/epan"
echo "📦 Installing saunafs.so to ${PERSONAL_CONFIG_DIR}"
mkdir -p "${PERSONAL_CONFIG_DIR}"
cp -v "${PLUGIN_PATH}" "${PERSONAL_CONFIG_DIR}"

VERSION_WITH_ONLY_MAJOR_MINOR=${WIRESHARK_VERSION%.*}
DESTINATION_DIR="/usr/lib/x86_64-linux-gnu/wireshark/plugins/${VERSION_WITH_ONLY_MAJOR_MINOR}/epan"

echo "Creating soft link to saunafs.so in ${DESTINATION_DIR}"
sudo ln -sv --force "${PERSONAL_CONFIG_DIR}/saunafs.so" "${DESTINATION_DIR}/saunafs.so"

echo "✅ SaunaFS plugin built and installed successfully!"


#!/usr/bin/env bash
# Single source of truth for building and installing NFS-Ganesha.
#
# The version lives in ganesha.env; ntirpc is resolved from that release's git
# submodule (no manual ntirpc pin to maintain). Used by the CI workflow and the
# Ganesha Docker image so they always deploy the same version, the same way.
#
# Usage: build-ganesha.sh [INSTALL_PREFIX]
#   INSTALL_PREFIX  CMAKE_INSTALL_PREFIX for the install (default: /usr).
#
# Runs the install step through sudo automatically when not already root.
set -euo pipefail

here=$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")
# shellcheck source=/dev/null
. "${here}/ganesha.env" # GANESHA_VERSION, GANESHA_GIT_URL

prefix="${1:-/usr}"

sudo=""
if [[ ${EUID} -ne 0 ]] && command -v sudo >/dev/null 2>&1; then
	sudo=sudo
fi

workdir=$(mktemp -d)
trap 'rm -rf "${workdir}"' EXIT
src="${workdir}/nfs-ganesha"

# Shallow clone at the pinned tag WITH submodules, so src/libntirpc is checked
# out at exactly the commit this Ganesha release pins. ntirpc therefore tracks
# the Ganesha version automatically.
git clone --depth 1 --branch "${GANESHA_VERSION}" \
	--recurse-submodules --shallow-submodules \
	"${GANESHA_GIT_URL}" "${src}"

cmake -B "${src}/build" "${src}/src" \
	-DCMAKE_C_FLAGS="-Wno-unused-function" \
	-DCMAKE_INSTALL_PREFIX="${prefix}" \
	-DUSE_9P=OFF \
	-DUSE_FSAL_CEPH=OFF \
	-DUSE_FSAL_GLUSTER=OFF \
	-DUSE_FSAL_GPFS=OFF \
	-DUSE_FSAL_KVSFS=OFF \
	-DUSE_FSAL_LIZARDFS=OFF \
	-DUSE_FSAL_LUSTRE=OFF \
	-DUSE_FSAL_PROXY_V3=OFF \
	-DUSE_FSAL_PROXY_V4=OFF \
	-DUSE_FSAL_RGW=OFF \
	-DUSE_FSAL_XFS=OFF \
	-DUSE_GSS=ON \
	-DUSE_NFS_RDMA=ON \
	-DUSE_MONITORING=OFF

${sudo} cmake --build "${src}/build" -j "$(($(nproc) * 3 / 4 + 1))" --target install

#!/usr/bin/env bash
set -eux -o pipefail
PROJECT_DIR="$(readlink -f "$(dirname "${BASH_SOURCE[0]}")/../..")"
WORKSPACE=${WORKSPACE:-"${PROJECT_DIR}"}
die() { echo "Error: $*" >&2; exit 1; }

test_filter="${1:-}"
[ -n "${test_filter}" ] || test_filter="GaneshaTests.*"
echo "test_filter: ${test_filter}"

export SAUNAFS_ROOT=${WORKSPACE}/install/leilfs
echo "SAUNAFS_ROOT: ${SAUNAFS_ROOT}"
export TEST_OUTPUT_DIR=${WORKSPACE}/test_output
echo "TEST_OUTPUT_DIR: ${TEST_OUTPUT_DIR}"
export TERM=xterm

killall -9 leil-tests || true
mkdir -m 777 -p "${TEST_OUTPUT_DIR}"
rm -rf "${TEST_OUTPUT_DIR:?}"/* || true
sudo rm -rf /mnt/ramdisk/* || true
[ -f "${SAUNAFS_ROOT}/bin/leil-tests" ] || \
	die "${SAUNAFS_ROOT}/bin/leil-tests" not found, did you build the project?
export PATH="${SAUNAFS_ROOT}/bin:${PATH}"
sudo sed -E -i '\,.*:\s+\$\{SAUNAFS_ROOT\s*:=.*,d' /etc/saunafs_tests.conf || true
echo ": \${SAUNAFS_ROOT:=${SAUNAFS_ROOT}}" | sudo tee -a /etc/saunafs_tests.conf >/dev/null || true
[ -f ${SAUNAFS_ROOT}/lib/ganesha/libfsalsaunafs.so ] || die 'Missing libfsalsaunafs.so'
sudo ln -sf ${SAUNAFS_ROOT}/lib/ganesha/libfsalsaunafs.so /usr/lib/ganesha/libfsalsaunafs.so
sudo mkdir -p /usr/lib/x86_64-linux-gnu/ganesha
sudo ln -sf ${SAUNAFS_ROOT}/lib/ganesha/libfsalsaunafs.so /usr/lib/x86_64-linux-gnu/ganesha/libfsalsaunafs.so
sudo --preserve-env=SAUNAFS_TEST_TIMEOUT_MULTIPLIER \
"${SAUNAFS_ROOT}/bin/leil-tests" --gtest_color=yes \
--gtest_filter="${test_filter}" --gtest_output=xml:"${TEST_OUTPUT_DIR}/ganesha_test_results.xml"

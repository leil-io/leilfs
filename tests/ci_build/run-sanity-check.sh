#!/usr/bin/env bash
set -eux -o pipefail
PROJECT_DIR="$(readlink -f "$(dirname "${BASH_SOURCE[0]}")/../..")"
WORKSPACE=${WORKSPACE:-"${PROJECT_DIR}"}
die() { echo "Error: $*" >&2; exit 1; }
declare -a test_extra_args=()
if [ -n "${1:-}" ]; then
	test_extra_args+=("--gtest_filter=${1}")
	shift 1
else
	test_extra_args+=("--gtest_filter=SanityChecks.*")
fi

while [ -n "${1:-}" ]; do
	test_extra_args+=("${1}")
	shift 1
done

echo "test_extra_args: ${test_extra_args[*]}"

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
sudo --preserve-env=SAUNAFS_TEST_TIMEOUT_MULTIPLIER \
"${SAUNAFS_ROOT}/bin/leil-tests" --gtest_color=yes \
--gtest_output=xml:"${TEST_OUTPUT_DIR}/sanity_test_results.xml" "${test_extra_args[@]}"

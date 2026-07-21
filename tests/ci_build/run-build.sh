#!/usr/bin/env bash
set -eux -o pipefail
PROJECT_DIR="$(readlink -f "$(dirname "${BASH_SOURCE[0]}")/../..")"

die() { echo "Error: $*" >&2; exit 1; }
warn() { echo "Warning: $*" >&2; }

usage() {
	cat <<-EOT
	Builds saunafs with different configurations

	Usage: run-build.sh [OPTION] [COMPILER]

	Options:
	  coverage   Build with parameters for coverage report
	  test       Build for test
	  release    Build with no debug info

	Compiler:
	  gcc        Use GCC (default)
	  clang      Use Clang
	EOT
	exit 1
}

declare -a CMAKE_SAUNAFS_ARGUMENTS=(
	-G 'Unix Makefiles'
	-DENABLE_CLIENT_LIB=ON
	-DENABLE_DOCS=ON
	-DENABLE_NFS_GANESHA=ON
	-DENABLE_URAFT=ON
	-DGSH_CAN_HOST_LOCAL_FS=ON
)

[ -n "${1:-}" ] || usage
declare build_type="${1}"
shift

declare compiler="${1:-gcc}"
shift || true

case "${compiler,,}" in
	gcc)
		;;
	clang)
		CMAKE_SAUNAFS_ARGUMENTS+=(
			-DCMAKE_C_COMPILER=clang
			-DCMAKE_CXX_COMPILER=clang++
		)
		;;
	*) die "Unsupported compiler: ${compiler}"
		;;
esac

declare build_dir
declare -a make_extra_args=("${@}")
if [ -n "${VCPKG_ROOT:-}" ]; then
	CMAKE_SAUNAFS_ARGUMENTS+=(
		-DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
	)
fi

case "${build_type,,}" in
	debug)
		CMAKE_SAUNAFS_ARGUMENTS+=(
			-DCMAKE_BUILD_TYPE=Debug
			-DCMAKE_INSTALL_PREFIX="${PROJECT_DIR}/install/leilfs/"
			-DENABLE_TESTS=ON
			-DCODE_COVERAGE=OFF
			-DSAUNAFS_TEST_POINTER_OBFUSCATION=ON
			-DENABLE_WERROR=ON
		)
		build_dir="${PROJECT_DIR}/build/saunafs-debug"
		make_extra_args+=( 'install' )
		;;
	coverage)
		CMAKE_SAUNAFS_ARGUMENTS+=(
			-DCMAKE_BUILD_TYPE=RelWithDebInfo
			-DCMAKE_INSTALL_PREFIX="${PROJECT_DIR}/install/leilfs/"
			-DENABLE_TESTS=ON
			-DCODE_COVERAGE=ON
			-DSAUNAFS_TEST_POINTER_OBFUSCATION=ON
			-DENABLE_WERROR=OFF
		)
		build_dir="${PROJECT_DIR}/build/saunafs-coverage"
		make_extra_args+=( 'install' )
		;;
	test)
		CMAKE_SAUNAFS_ARGUMENTS+=(
			-DCMAKE_BUILD_TYPE=RelWithDebInfo
			-DCMAKE_INSTALL_PREFIX="${PROJECT_DIR}/install/leilfs/"
			-DENABLE_TESTS=ON
			-DCODE_COVERAGE=OFF
			-DSAUNAFS_TEST_POINTER_OBFUSCATION=ON
			-DENABLE_WERROR=ON
		)
		build_dir="${PROJECT_DIR}/build/saunafs"
		make_extra_args+=( 'install' )
		;;
	release)
		CMAKE_SAUNAFS_ARGUMENTS+=(
			-DCMAKE_BUILD_TYPE=RelWithDebInfo
			-DCMAKE_INSTALL_PREFIX=/
			-DENABLE_TESTS=OFF
			-DCODE_COVERAGE=OFF
			-DSAUNAFS_TEST_POINTER_OBFUSCATION=OFF
			-DENABLE_WERROR=OFF
		)
		build_dir="${PROJECT_DIR}/build/saunafs-release"
		;;
	*) die "Unsupported build type: ${build_type}"
		;;
esac

if [ -n "${PACKAGE_VERSION:-}" ]; then
	CMAKE_SAUNAFS_ARGUMENTS+=( -DPACKAGE_VERSION="${PACKAGE_VERSION}" )
fi

declare -a EXTRA_ARGUMENTS=("${@}")
# shellcheck disable=SC2115
rm -r "${build_dir:?}"/{,.}* 2>/dev/null || true
cmake -B "${build_dir}" \
	"${CMAKE_SAUNAFS_ARGUMENTS[@]}" \
	"${EXTRA_ARGUMENTS[@]}" -S "${PROJECT_DIR}"

nice make -C "${build_dir}" -j"$(nproc)" "${make_extra_args[@]}"

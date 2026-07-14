#!/usr/bin/env bash
set -eu -o pipefail
readonly self="$(readlink -f "${BASH_SOURCE[0]}")"
readonly script_dir="$(readlink -f "$(dirname "${self}")")"
readonly me="$(basename "${self}")"

die() { echo "Error: ${*}" >&2; usage; exit 1; }
run() { echo "[${me}] ${*}" >&2; "${@}"; }

get_options() {
	find "${script_dir}" -type f -name 'Dockerfile.*' \
		-exec sh -c 'basename "${1}" | cut -d. -f2-' shell {} \; | \
	sort -u
}

hint_used_variables() {
	grep -oP '\$\{?[A-Z][A-Z0-9_]+' "${self}" | sed -E 's/\$\{?//' | sort -u | grep -v BASH_SOURCE
}

usage() {
	local -a options=()
	local -a env_vars=()
	local DOCKER_REGISTRY_USERNAME
	local DOCKER_REGISTRY_PASSWORD
	while read -r option; do
		options+=("    ${option}")
	done < <(get_options)
	while read -r var; do
		DOCKER_REGISTRY_USERNAME="${DOCKER_REGISTRY_USERNAME+********}"
		DOCKER_REGISTRY_PASSWORD="${DOCKER_REGISTRY_PASSWORD+********}"
		env_vars+=("    ${var}=${!var:-}")
	done < <(hint_used_variables)

	local IFS=$'\n'
	cat >&2 <<-EOF
		Usage: ${me} <option>

		Options:
		    --help, -h		Show this help
		    ganesha
		${options[*]}

		Environment variables:
		${env_vars[*]}
		You can use "${WORKSPACE}/.env" file to set these variables.
	EOF
}

parse_true() {
	[ -n "${1:-}" ] || return 1
	case "${1,,}" in
		1|true|t|yes|y|on|enabled)
			return 0
			;;
		*)
			return 1
			;;
	esac
}

load_env() {
	local env_file
	while [ -n "${1:-}" ]; do
		env_file="${1}"
		shift
		if [ -f "${env_file}" ]; then
			set -a
			# shellcheck source=./.env
			. <(sed -E '/^\s*(#|$)/d;s/\s*=\s*/=/' "${env_file}")
			set +a
		fi
	done
}

get_commit_id() {
	git rev-parse --short HEAD
}

get_branch_name() {
	git rev-parse --abbrev-ref HEAD
}

getBuildTimestamp() {
	date -u +"%Y%m%d-%H%M%S"
}

get_partial_tag() {
	local branch="${1}"
	local custom_tag="${2}"
	local commit_id="${3:-latest}"
	echo "${branch}-${custom_tag}-${commit_id}"
}

docker_login() {
	if grep -q "${DOCKER_INTERNAL_REGISTRY}" ~/.docker/config.json; then
		echo "Already logged in to ${DOCKER_INTERNAL_REGISTRY}"
	else
		local -a docker_login_args=()
		[ -z "${DOCKER_REGISTRY_USERNAME}" ] || docker_login_args+=(--username "${DOCKER_REGISTRY_USERNAME}")
		[ -z "${DOCKER_REGISTRY_PASSWORD}" ] || docker_login_args+=(--password "${DOCKER_REGISTRY_PASSWORD}")
		docker login "${DOCKER_INTERNAL_REGISTRY}" "${docker_login_args[@]}"
	fi
}

: "${PROJECT_DIR:="$(readlink -f "$(dirname "${self}")/../..")"}"
: "${WORKSPACE:="${PROJECT_DIR}"}"

load_env "${WORKSPACE}/.env"

: "${PROJECT_NAME:="$(basename "${PROJECT_DIR}")"}"
: "${DOCKER_INTERNAL_REGISTRY:=registry.ci.leil.io}"
: "${DOCKER_REGISTRY_USERNAME:=}"
: "${DOCKER_REGISTRY_PASSWORD:=}"
: "${DOCKER_BASE_IMAGE:=ubuntu:24.04}"
: "${DOCKER_IMAGE_GROUP_ID:=$(id -g)}"
: "${DOCKER_IMAGE_USER_ID:=$(id -u)}"
: "${DOCKER_IMAGE_USERNAME:=$(id -un)}"
: "${DOCKER_ENABLE_PULL_CACHE_IMAGE:=false}"
: "${DOCKER_ENABLE_PUSH_IMAGE:=false}"
: "${DOCKER_IMAGE:="${PROJECT_NAME}"}"
DOCKER_IMAGE="${DOCKER_IMAGE//[\/.]/-}"
DOCKER_IMAGE="${DOCKER_IMAGE,,}"
: "${BRANCH_NAME:="$(get_branch_name)"}"

declare -a build_extra_args=()

option="${1:-}"
[ -n "${option}" ] || die "Option not specified"
shift
case "${option,,}" in
	--help | -h)
		usage
		exit 0
		;;
	--list | -l)
		get_options
		exit 0
		;;
	--env | -e)
		hint_used_variables
		exit 0
		;;
	ganesha)
		docker_context_dir="${PROJECT_DIR}/tests/ci_build/ganesha"
		# All bases use the source-building Dockerfile so the image ships the
		# pinned Ganesha V9.15 (+ its ntirpc).
		dockerfile="${docker_context_dir}/Dockerfile"
		;;
	*)
		docker_context_dir="${PROJECT_DIR}"
		dockerfile="$(readlink -f "${script_dir}/Dockerfile.${option}")"
		[ -f "${dockerfile}" ] || die "Dockerfile not found: ${dockerfile}"
		;;
esac

tag_option="${DOCKER_BASE_IMAGE//:/-}"
if [ -n "${DOCKER_CUSTOM_TAG:-}" ]; then
	tag_option="${DOCKER_CUSTOM_TAG}"
fi
tag_option="${tag_option##*/}"

latest_tag="$(get_partial_tag "${BRANCH_NAME}" "${tag_option}")"
container_tag="$(get_partial_tag "${BRANCH_NAME}" "${tag_option}" "$(get_commit_id)")"

[ -n "${DOCKER_INTERNAL_REGISTRY}" ] || die "DOCKER_REGISTRY not set"
[ -n "${DOCKER_IMAGE}" ] || die "DOCKER_IMAGE not set"

build_extra_args+=(--build-arg BASE_IMAGE="${DOCKER_BASE_IMAGE}")

if parse_true "${DOCKER_ENABLE_PULL_CACHE_IMAGE}"; then
	docker_login
	run docker pull "${DOCKER_INTERNAL_REGISTRY}/${DOCKER_IMAGE}:${latest_tag}" 2>/dev/null || true
	build_extra_args+=(--cache-from "${DOCKER_INTERNAL_REGISTRY}/${DOCKER_IMAGE}:${latest_tag}")
fi

run docker buildx build --pull --load --progress=plain \
	--build-arg GROUP_ID="${DOCKER_IMAGE_GROUP_ID}" \
	--build-arg USER_ID="${DOCKER_IMAGE_USER_ID}" \
	--build-arg USERNAME="${DOCKER_IMAGE_USERNAME}" \
	"${build_extra_args[@]}" \
	--tag "${DOCKER_INTERNAL_REGISTRY}/${DOCKER_IMAGE}:${container_tag}" \
	--tag "${DOCKER_INTERNAL_REGISTRY}/${DOCKER_IMAGE}:${latest_tag}" \
	--file "${dockerfile}" \
	"${docker_context_dir}"

if parse_true "${DOCKER_ENABLE_PUSH_IMAGE}"; then
	docker_login
	run docker push "${DOCKER_INTERNAL_REGISTRY}/${DOCKER_IMAGE}:${container_tag}"
	run docker push "${DOCKER_INTERNAL_REGISTRY}/${DOCKER_IMAGE}:${latest_tag}"
fi

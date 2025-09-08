#!/usr/bin/env bash
set -xeu -o pipefail

## This script installs the dependencies specified in vcpkg.json
## and pins the versions of those dependencies.
## Usually, you should run `vcpkg install` on a daily basis,
## and this script should be run after updating the dependencies in vcpkg.json

usage() {
  cat <<EOF
Usage: $0 [-u|--update-baseline]

Options:
  -u, --update-baseline  Update the baseline for versioning

Examples:
  $0
  $0 -u
EOF
}

: "${UPDATE_BASELINE:=false}"

die() {	echo "Error: $@" >&2; exit 1; }

if ! command -v jq >/dev/null 2>&1; then
  die "jq is not found. Please install jq."
fi

if [ ! -f "vcpkg.json" ]; then
	die "vcpkg.json not found"
fi

while [ -n "${1:-}" ]; do
	case "${1}" in
	-u|--update-baseline)
		UPDATE_BASELINE=true
		shift
		;;
	-h|--help)
		usage
		exit 0
		;;
	*)
		usage
		exit 1
		;;
	esac
done

if [ "${UPDATE_BASELINE}" = "true" ]; then
	# Set the baseline for the versioning
	vcpkg x-update-baseline --add-initial-baseline
fi

# Install dependencies specified in vcpkg.json
vcpkg install --x-asset-sources=clear --binarysource=clear --x-install-root=./vcpkg_installed

# Pin the dependencies versions
# Get the versions from the current list of installed packages
overrides_attribute="$(vcpkg list --x-json | jq -c '[.[] | {name:.package_name,version}]')"
if [ "${overrides_attribute}" == "[]" ]; then
	die "No dependencies found"
fi
# Remove the overrides attribute from the vcpkg.json file
mv vcpkg.json vcpkg.json.bak
source_object="$(jq -r 'del(.overrides)' vcpkg.json.bak)"
# Add the overrides attribute with the versions from the current list of installed packages
echo "${source_object}" | jq --argjson overrides "{\"overrides\": ${overrides_attribute}}" '. * $overrides' > vcpkg.json

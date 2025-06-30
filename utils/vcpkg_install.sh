#!/usr/bin/env bash
set -xeu -o pipefail

# Set the baseline for the versioning
# vcpkg x-update-baseline --add-initial-baseline

# Install dependencies specified in vcpkg.json
# vcpkg install
vcpkg install --x-asset-sources=clear --binarysource=clear --x-install-root=./vcpkg_installed

# Pin the dependencies versions
# Get the versions from the current list of installed packages
overrides_attribute="$(vcpkg list --x-json | jq -c '[.[] | {name:.package_name,version}]')"
if [ "${overrides_attribute}" == "[]" ]; then
	exit 0
fi
# Remove the overrides attribute from the vcpkg.json file
mv vcpkg.json vcpkg.json.bak
source_object="$(jq -r 'del(.overrides)' vcpkg.json.bak)"
# Add the overrides attribute with the versions from the current list of installed packages
echo "${source_object}" | jq --argjson overrides "{\"overrides\": ${overrides_attribute}}" '. * $overrides' > vcpkg.json

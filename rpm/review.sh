#!/usr/bin/env bash

set -euo pipefail

# Uses built RPM packages in results_*/ to generate a review in
# review-*/review.txt

if ! read NAME VERSION < <(rpmspec -q --qf "%{NAME} %{VERSION}\n" *.spec | head -1); then
    echo "ERROR: Failed to parse NAME or VERSION from spec file." >&2
    exit 1
fi
if [ -z "${NAME:-}" ] || [ -z "${VERSION:-}" ]; then
    echo "ERROR: Failed to parse NAME or VERSION from spec file." >&2
    exit 1
fi

REVIEW_DIR="review-${NAME}"
REVIEW_WORKDIR=""

cleanup() {
    if [ -n "${REVIEW_WORKDIR}" ]; then
        rm -rf "${REVIEW_WORKDIR}"
    fi
}
trap cleanup EXIT

REVIEW_WORKDIR=$(mktemp -d --tmpdir "${NAME}-review.XXXXXX")

echo "Cleaning old review results..."
rm -rf "${REVIEW_DIR}"

RESULTS_DIR="results_${NAME}/${VERSION}"
RELEASE_DIR=$(find "${RESULTS_DIR}/" -maxdepth 1 -mindepth 1 -type d -print -quit 2>/dev/null || true)

if [ -z "${RELEASE_DIR}" ]; then
    echo "ERROR: No build results found in ${RESULTS_DIR}/"
    exit 1
fi

echo "Found build results in: ${RELEASE_DIR}"
echo "Copying resulting source and rpm packages to ${REVIEW_WORKDIR}..."
find "${RELEASE_DIR}/" -name "*.rpm" ! -name "*debug*" -exec cp -f {} "${REVIEW_WORKDIR}/" \;
cp -f "${NAME}.spec" "${REVIEW_WORKDIR}/"

echo "Running Fedora Review..."
fedora_review_status=0
(
    cd "${REVIEW_WORKDIR}"
    fedora-review -n "${NAME}" -p -v
) || fedora_review_status=$?

if [ -d "${REVIEW_WORKDIR}/${REVIEW_DIR}" ]; then
    cp -a "${REVIEW_WORKDIR}/${REVIEW_DIR}" .
fi

if [ "${fedora_review_status}" -ne 0 ]; then
    echo "ERROR: Fedora Review failed with exit code ${fedora_review_status}"
    exit "${fedora_review_status}"
fi

echo "Review concluded."

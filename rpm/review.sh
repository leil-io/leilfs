#!/usr/bin/env bash

set -euo pipefail

# Uses built RPM packages in results_*/ to generate a review in
# review-*/review.txt

read NAME VERSION < <(rpmspec -q --qf "%{NAME} %{VERSION}\n" *.spec | head -1)
REVIEW_DIR="review-${NAME}"
REVIEW_WORKDIR=$(mktemp -d -t "${NAME}-review.XXXXXX")

cleanup() {
    rm -rf "${REVIEW_WORKDIR}"
}
trap cleanup EXIT

echo "Cleaning old review results..."
rm -rf "${REVIEW_DIR}"

RELEASE_DIR=$(find "results_${NAME}/${VERSION}/" -maxdepth 1 -mindepth 1 -type d | head -1)

if [ -z "${RELEASE_DIR}" ]; then
    echo "ERROR: No build results found in results_${NAME}/${VERSION}/"
    exit 1
fi

echo "Found build results in: ${RELEASE_DIR}"
echo "Copying resulting source and rpm packages to ${REVIEW_WORKDIR}..."
cp -f "${RELEASE_DIR}"/*.src.rpm "${REVIEW_WORKDIR}/"
find "${RELEASE_DIR}/" -name "*.rpm" ! -name "*debug*" -exec cp -f {} "${REVIEW_WORKDIR}/" \;
cp -f "${NAME}.spec" "${REVIEW_WORKDIR}/"

echo "Running Fedora Review..."
(
    cd "${REVIEW_WORKDIR}"
    fedora-review -n "${NAME}" -p -v
)

if [ -d "${REVIEW_WORKDIR}/${REVIEW_DIR}" ]; then
    cp -a "${REVIEW_WORKDIR}/${REVIEW_DIR}" .
fi

echo "Review concluded."

#!/usr/bin/env bash

set -euo pipefail

# Builds the RPM package locally

FILTERS=filters.toml
SOURCE_REF=${1:-HEAD}

read NAME VERSION RELEASE < <(rpmspec -q --qf "%{NAME} %{VERSION} %{RELEASE}\n" *.spec | head -1)
SOURCE_TARBALL=v${VERSION}.tar.gz
REPO_ROOT=$(git rev-parse --show-toplevel 2>/dev/null || true)

resolve_source_ref() {
    local ref=$1
    local candidate

    for candidate in \
        "${ref}" \
        "origin/${ref}" \
        "refs/heads/${ref}" \
        "refs/remotes/origin/${ref}" \
        "refs/tags/${ref}"; do
        if git -C "${REPO_ROOT}" rev-parse --verify --quiet "${candidate}^{commit}" >/dev/null; then
            git -C "${REPO_ROOT}" rev-parse --verify "${candidate}^{commit}"
            return
        fi
    done

    echo "ERROR: Git reference not found: ${ref}" >&2
    exit 1
}

create_source_tarball() {
    local resolved_ref short_ref

    if [ -z "${REPO_ROOT}" ]; then
        echo "Getting source code tarball from spec..."
        spectool -g ${NAME}.spec
        return
    fi

    resolved_ref=$(resolve_source_ref "${SOURCE_REF}")
    short_ref=$(git -C "${REPO_ROOT}" rev-parse --short "${resolved_ref}")

    echo "Creating source code tarball from Git reference: ${SOURCE_REF} (${short_ref})"
    git -C "${REPO_ROOT}" archive \
        --format=tar.gz \
        --prefix="${NAME}-${VERSION}/" \
        --output="${PWD}/${SOURCE_TARBALL}" \
        "${resolved_ref}"
}

echo "Packaging ${NAME} ${VERSION} ${RELEASE}..."

echo "Cleaning old build artifacts and sources..."
rm -f ${NAME}-${VERSION}-${RELEASE}.src.rpm
rm -rf results_${NAME}
rm -f ${SOURCE_TARBALL}

echo "Running rpmlint on ${NAME}.spec..."
if rpmlint -c ${FILTERS} ${NAME}.spec; then
    echo "${NAME}.spec check successful."
    create_source_tarball
    echo "Building packages..."
    # If BuildRequires was changed - reinstall every BuildRequires
    echo "Running local mockbuild..."
    fedpkg mockbuild -- --clean -v
    echo "Running rpmlint in results_${NAME}/${VERSION}/${RELEASE}/..."
    if rpmlint -c ${FILTERS} results_${NAME}/${VERSION}/${RELEASE}/*.x86_64.rpm; then
        echo "Build finished successfully."
    else
        echo "ERROR: Build finished unsuccessfully"
        exit 1
    fi
else
    echo "ERROR: Linting check unsuccessful."
    exit 1
fi

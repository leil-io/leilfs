#!/usr/bin/env bash

set -euo pipefail

# Builds the RPM package locally

FILTERS=filters.toml
SOURCE_REF=${1:-HEAD}

if ! read NAME VERSION RELEASE < <(rpmspec -q --qf "%{NAME} %{VERSION} %{RELEASE}\n" *.spec | head -1); then
    echo "ERROR: Failed to parse NAME, VERSION, or RELEASE from spec file." >&2
    exit 1
fi
if [ -z "${NAME:-}" ] || [ -z "${VERSION:-}" ] || [ -z "${RELEASE:-}" ]; then
    echo "ERROR: Failed to parse NAME, VERSION, or RELEASE from spec file." >&2
    exit 1
fi

SOURCE_TARBALL="v${VERSION}.tar.gz"
REPO_ROOT_ERROR=$(mktemp)
trap 'rm -f "${REPO_ROOT_ERROR}"' EXIT
REPO_ROOT=$(git rev-parse --show-toplevel 2>"${REPO_ROOT_ERROR}" || true)

resolve_source_ref() {
    local ref=$1
    local candidate
    local resolved

    for candidate in \
        "${ref}" \
        "origin/${ref}" \
        "refs/heads/${ref}" \
        "refs/remotes/origin/${ref}" \
        "refs/tags/${ref}"; do
        if resolved=$(git -C "${REPO_ROOT}" rev-parse --verify --quiet "${candidate}^{commit}" 2>/dev/null); then
            echo "${resolved}"
            return 0
        fi
    done

    echo "ERROR: Git reference not found: ${ref}" >&2
    return 1
}

create_source_tarball() {
    local resolved_ref short_ref

    if [ -z "${REPO_ROOT}" ]; then
        if grep -qi "dubious ownership" "${REPO_ROOT_ERROR}"; then
            echo "ERROR: Git refused this checkout due to dubious ownership." >&2
            echo "Run the build without sudo or configure Git safe.directory for this checkout." >&2
            exit 1
        fi

        if [ "${SOURCE_REF}" != "HEAD" ]; then
            echo "ERROR: ${SOURCE_REF} requested but not inside a Git checkout." >&2
            exit 1
        fi

        echo "WARNING: not inside a Git checkout; downloading source tarball from spec." >&2
        spectool -g "${NAME}.spec"
        return
    fi

    resolved_ref=$(resolve_source_ref "${SOURCE_REF}")
    if [ "${resolved_ref}" != "$(git -C "${REPO_ROOT}" rev-parse HEAD)" ]; then
        echo "ERROR: ${SOURCE_REF} does not match the current checkout; please check out that ref or run ./build.sh without an argument." >&2
        exit 1
    fi

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
rm -f "${NAME}-${VERSION}-${RELEASE}.src.rpm"
rm -rf "results_${NAME}"
rm -f "${SOURCE_TARBALL}"

echo "Running rpmlint on ${NAME}.spec..."
if rpmlint -c "${FILTERS}" "${NAME}.spec"; then
    echo "${NAME}.spec check successful."
    create_source_tarball
    echo "Building packages..."
    # If BuildRequires was changed - reinstall every BuildRequires
    echo "Running local mockbuild..."
    fedpkg mockbuild -- --clean -v
    echo "Running rpmlint in results_${NAME}/${VERSION}/${RELEASE}/..."
    if rpmlint -c "${FILTERS}" "results_${NAME}/${VERSION}/${RELEASE}"/*.x86_64.rpm; then
        echo "Build finished successfully."
    else
        echo "ERROR: Build finished unsuccessfully"
        exit 1
    fi
else
    echo "ERROR: Linting check unsuccessful."
    exit 1
fi

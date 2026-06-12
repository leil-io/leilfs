#!/usr/bin/env bash

# Builds the RPM package locally

FILTERS=filters.toml

read NAME VERSION RELEASE < <(rpmspec -q --qf "%{NAME} %{VERSION} %{RELEASE}\n" *.spec | head -1)

echo "Packaging ${NAME} ${VERSION} ${RELEASE}..."

echo "Cleaning old build artifacts and sources..."
rm -f ${NAME}-${VERSION}-${RELEASE}.src.rpm
rm -rf results_${NAME}
rm -f v${VERSION}.tar.gz

echo "Running rpmlint on ${NAME}.spec..."
if rpmlint -c ${FILTERS} ${NAME}.spec; then
    echo "${NAME}.spec check successful."
    echo "Getting source code tarball..."
    spectool -g ${NAME}.spec
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

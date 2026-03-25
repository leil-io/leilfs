#!/usr/bin/env bash

# Uses built RPM packages in results_*/ to generate a review in
# review-*/review.txt

read NAME VERSION RELEASE < <(rpmspec -q --qf "%{NAME} %{VERSION}\n" *.spec | head -1)

echo "Cleaning old review results..."
rm -rf review-${NAME}

RELEASE_DIR=$(find results_${NAME}/${VERSION}/ -maxdepth 1 -mindepth 1 -type d | head -1)

if [ -z "$RELEASE_DIR" ]; then
    echo "ERROR: No build results found in results_${NAME}/${VERSION}/"
    exit 1
fi

echo "Found build results in: ${RELEASE_DIR}"
echo "Copying resulting source and rpm packages..."
cp -f ${RELEASE_DIR}/*.src.rpm .
find ${RELEASE_DIR}/ -name "*.rpm" ! -name "*debug*" -exec cp -f {} . \;

echo "Running Fedora Review..."
fedora-review -n ${NAME} -p -v

echo "Review concluded, removing source and rpm packages..."
rm -f *.src.rpm
rm -f *.rpm

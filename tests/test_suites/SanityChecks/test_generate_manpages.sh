# A full cmake configure dominates this test at about 50 seconds and does not shrink by turning
# features off.
timeout_set 150 seconds

assert_program_installed gem
assert_program_installed rsync

# Work on a copy so the doc build cannot touch the checkout. build/ and vcpkg/ are gigabytes of
# artifacts manpage generation never reads, and copying them dominated the runtime.
SAUNAFS_FOLDER=$(basename "${SOURCE_DIR}")
rsync -a --exclude=/build --exclude=/vcpkg --exclude=/vcpkg_installed \
	"${SOURCE_DIR}/" "${TEMP_DIR}/${SAUNAFS_FOLDER}/"
cd "${TEMP_DIR}/${SAUNAFS_FOLDER}" || exit 1

BUILD_DIR="${TEMP_DIR}/build_saunafs_doc_$(date +%s)"
cmake -B "${BUILD_DIR}" -S .
make -C "${BUILD_DIR}/doc" 2>&1 | tee "${TEMP_DIR}/build-doc.log"

# Legacy SyntaxWarning came from Python asciidoc; asciidoctor should never emit it.
pattern="SyntaxWarning: invalid escape sequence"
if grep -q "${pattern}" "${TEMP_DIR}/build-doc.log"; then
	echo "Test failed: 'SyntaxWarning: invalid escape sequence' found in doc build output"
	exit 1
fi

# Sanity check that asciidoctor actually produced manpages
if ! find "${BUILD_DIR}/doc" -type f -name '*.1' -o -name '*.5' -o -name '*.7' -o -name '*.8' | grep -q .; then
	echo "Test failed: no manpages generated in ${BUILD_DIR}/doc"
	exit 1
fi

echo "Test passed: no invalid escape warnings and manpages generated successfully"

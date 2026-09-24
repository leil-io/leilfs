# Configuring the whole project dominates this test at about 50 seconds, and turning features
# off barely dents it, so the budget has to cover a full configure. The default of 30 seconds
# was never raised for this test and left no headroom. Most of the configure is serial, so the
# base has to suit a machine that sets no multiplier; slower or loaded agents are covered by
# SAUNAFS_TEST_TIMEOUT_MULTIPLIER, which is 5 on the CI agent this test timed out on.
timeout_set 150 seconds

assert_program_installed gem
assert_program_installed rsync

# Work on a copy, so nothing the doc build does can touch the checkout. Only the sources are
# needed: build/ and vcpkg/ are gigabytes of artifacts that manpage generation never reads, and
# copying them was both the slowest step here and the one that varied most with disk load.
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

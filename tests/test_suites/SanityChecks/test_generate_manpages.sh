assert_program_installed gem

# Move to a temp copy of the source tree
cp -r "${SOURCE_DIR}" "${TEMP_DIR}"
SAUNAFS_FOLDER=$(basename "${SOURCE_DIR}")
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

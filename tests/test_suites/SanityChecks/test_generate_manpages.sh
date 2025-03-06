assert_program_installed a2x
timeout_set 2 minutes

# Change to saunafs SOURCE_DIR
cp -r "${SOURCE_DIR}" "${TEMP_DIR}"

# Fetch the name of the saunafs folder
SAUNAFS_FOLDER=$(basename "${SOURCE_DIR}")
cd "${TEMP_DIR}/${SAUNAFS_FOLDER}" || exit 1

# Create a unique build directory
BUILD_DIR="${TEMP_DIR}/build_saunafs_doc_$(date +%s)"

# Run the build process and capture the output
cmake -B "${BUILD_DIR}" -S .
make -C "${BUILD_DIR}/doc" 2>&1 | tee "${TEMP_DIR}/build-doc.log"

# Check for the presence of 'SyntaxWarning: invalid escape sequence' lines
pattern="SyntaxWarning: invalid escape sequence"
if grep -q "${pattern}" "${TEMP_DIR}/build-doc.log"; then
	echo "Test failed: 'SyntaxWarning: invalid escape sequence' found in a2x output"
	exit 1
else
	echo "Test passed: No 'SyntaxWarning: invalid escape sequence' found in a2x output"
fi

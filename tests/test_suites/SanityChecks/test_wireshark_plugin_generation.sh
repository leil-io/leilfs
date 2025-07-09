assert_program_installed python3

cd "${TEMP_DIR}"
cp -av "${SOURCE_DIR}/utils/wireshark/plugins/epan/saunafs" .
output_file=saunafs/packet-saunafs.c
SFSCommunication_h="${SOURCE_DIR}/src/protocol/SFSCommunication.h"

test_wireshark_version() {
    local version=$1
    local expected_string=$2
    local unexpected_string=$3

    rm -f ${output_file}
    assert_success saunafs/generate.sh "${SFSCommunication_h}" "${version}"
    assert_success test -s ${output_file}
    assert_success grep --quiet "${expected_string}" ${output_file}
    assert_failure grep --quiet "${unexpected_string}" ${output_file}
}

# Run all test cases
test_wireshark_version "4.0.0" "tvb_get_guint8" "tvb_get_uint8"
test_wireshark_version "4.5.0" "tvb_get_uint8" "tvb_get_guint8"

rm -f ${output_file}

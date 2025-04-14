cd "$TEMP_DIR"
cp -av "${SOURCE_DIR}/utils/wireshark/plugins/epan/saunafs" .
rm -f saunafs/*.c

if is_program_installed python3 ; then
	assert_success saunafs/generate.sh "$SOURCE_DIR/src/protocol/SFSCommunication.h"
	assert_success test -s saunafs/packet-saunafs.c
else
	echo "python3 is not installed on your system hence wireshark plugin won't be build."
fi

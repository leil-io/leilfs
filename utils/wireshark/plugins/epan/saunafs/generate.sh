#!/usr/bin/env bash
set -eu

if (( $# != 2 )); then
	echo "Usage: $0 path/to/SFSCommunication.h wireshark_version"
	exit 1
fi

WIRESHARK_VERSION="${2}"

input_file=$(readlink -m "$1")
cd "$(dirname "$0")"

# Generate the includes.h file which properly includes all the definitions of SaunaFS constants
{
	echo "#define PROTO_BASE 0"
	echo "#define SFSBLOCKSINCHUNK 1024"
	echo "#define SFSBLOCKSIZE 65536"
	echo "#define SAUNAFS_WIRESHARK_PLUGIN 1"
	echo "#include \"$input_file\"" # SFSCommunication.h
} > includes.h

# Generate the packet-saunafs.c file
python3 make_dissector.py < "$input_file" > packet-saunafs.c "${WIRESHARK_VERSION}"

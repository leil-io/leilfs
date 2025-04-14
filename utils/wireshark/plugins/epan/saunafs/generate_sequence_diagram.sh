#!/usr/bin/env bash

# Declare associative array for token mapping
declare -A tokens=(
    [AN]=ANY
    [CL]=CLIENT
    [CS]=CHUNKSERVER
    [MA]=MASTER
    [ML]=METALOGGER
    [TS]=TAPESERVER
)
# Sorted keys array
declare -a sorted_keys=(CL MA CS ML TS AN)

PLOT=false

usage() {
    cat <<EOT
USAGE:
  ${0} path/to/SFSCommunication.h list_of_arguments [--plot]

All arguments should be passed in the following way: ARG_NAME=value
List of arguments:
  PARTICIPANTS   - optional filter, e.g., 'AN|CL|CS|MA|ML|TS'
  ACTIONS        - optional filter, e.g., 'CLTOMA|GET_CHUNK|WRITE_END'
  --plot         - optional flag to generate a graphical flow chart

Example to plot diagram with communication between Client and ChunksSever:
  ${0} ./src/protocol/SFSCommunication.h PARTICIPANTS='CL|CS' --plot
EOT
    exit 1
}

# Validate the presence of required external commands
for cmd in grep awk sed dot; do
    if ! command -v "${cmd}" &> /dev/null; then
        echo "Error: ${cmd} is required but not installed. Aborting."
        exit 1
    fi
done

# Parse command line arguments
for ARGUMENT in "$@"; do
    if [[ "${ARGUMENT}" == "--plot" ]]; then
        PLOT=true
        continue
    fi

    KEY=$(echo "${ARGUMENT}" | cut -f1 -d=)
    VALUE=$(echo "${ARGUMENT}" | cut -f2 -d=)

    case "${KEY}" in
        PARTICIPANTS) PARTICIPANTS=${VALUE} ;;
        ACTIONS) ACTIONS=${VALUE} ;;
        *) echo "Warning: Unknown argument ${KEY}" ;;
    esac
done

# Validate the source file argument
source_file="${1:-}"
if [ -z "${source_file}" ]; then
    echo "Error: Source file path is required."
    usage
fi
if [ ! -f "${source_file}" ]; then
    echo "Error: Source file does not exist at '${source_file}'."
    exit 1
fi

# Optional filters
participant_filter="${PARTICIPANTS:-}"
action_filter="${ACTIONS:-}"

# Filter tokens based on participant filter
if [ -n "${participant_filter}" ]; then
    for key in "${!tokens[@]}"; do
        if ! echo "${participant_filter}" | grep -q '\b'"${key}"'\b'; then
            unset "tokens[${key}]"
        fi
    done
fi

# Print the header for the sequence diagram
print_header() {
    echo "title SaunaFS Protocol"
    echo
    for key in "${sorted_keys[@]}"; do
        if [ -n "${tokens[${key}]}" ]; then
            echo "participant ${tokens[${key}]}"
        fi
    done
    echo
}

get_key_disjunction() {
    local key_list=()
    for key in "${sorted_keys[@]}"; do
        key_list+=("${key}")
    done
    echo "${key_list[*]}" | tr ' ' '|'
}

get_search_pattern() {
    ored_keys="$(get_key_disjunction)"
    echo '('"${ored_keys}"')TO('"${ored_keys}"')'
}

get_transformation_pattern() {
    local search_pattern
    search_pattern="$(get_search_pattern)"
    local transform_pattern='s/(SAU_)?'"${search_pattern}"'/\2->\3:\0/;'
    for key in "${!tokens[@]}"; do
        transform_pattern+='s/(^|>)'"${key}"'/\1'"${tokens[${key}]}"'/g;'
    done
    echo "${transform_pattern}"
}

# Extract the sequence diagram from the source file
extract_sequence() {
    local search_pattern
    search_pattern="$(get_search_pattern)"
    local transform_pattern
    transform_pattern="$(get_transformation_pattern)"

    cat "${source_file}" \
        | grep -Eo '#define\s+\w+' \
        | awk '{print $2}' \
        | grep -E "${search_pattern}" \
        | sed -E "${transform_pattern}"
}

generate_dot_format() {
    local -a participants
    IFS='|' read -r -a participants <<< "${PARTICIPANTS}"
    local -a sorted_participants
    IFS=$'\n' read -r -a sorted_participants <<< "$(sort <<< "${participants[*]}")"
    unset IFS

    local filename_participants
    filename_participants=$(IFS=_; echo "saunafs_data_flow_participants_${sorted_participants[*]}"; IFS=$' \t\n')
    local output_file="${filename_participants}.png"

    echo "Generating graph in DOT format and saving as ${output_file}"

    {
        echo "digraph G {"
        echo "rankdir=LR;"

        if [ -z "${action_filter}" ]; then
            extract_sequence_to_dot
        else
            extract_sequence_to_dot | grep -E "${action_filter}"
        fi

        echo "}"
    } | dot -Tpng -o "${output_file}"
}

# Convert the sequence to DOT format
extract_sequence_to_dot() {
    local search_pattern
    search_pattern="$(get_search_pattern)"
    local transform_pattern
    transform_pattern="$(get_transformation_pattern)"

    extract_sequence | while IFS= read -r line; do
        echo "\"${line%%->*}\" -> \"${line##*->}\" [label=\"${line%%:*}\"];"
    done
}

# Main function to control the script flow
main() {
    print_header

    if ${PLOT}; then
        generate_dot_format
    else
        if [ -z "${action_filter}" ]; then
            extract_sequence
        else
            extract_sequence | grep -E "${action_filter}"
        fi
    fi
}

main

function goal_to_part_count() {
	local goal="${1}"
	echo $(($(echo ${goal} | sed -e 's/xor/1/g' | tr -d [a-z] | sed -e 's/\B/+/g')))
}

function filesize_to_chunk_count() {
	local filesize=${1}
	filesize=$(parse_si_suffix ${filesize})
	local chunksize=$(parse_si_suffix 64M)
	local chunksize_complement=$((chunksize - 1))
	local chunk_count=$(((filesize + chunksize_complement) / chunksize))
	echo ${chunk_count}
}

function redundant_parts() {
	local goal=${1}
	local rp=""
	case "${goal}" in
		ec*)
			rp="${goal: -1}"
		;;
		xor*)
			rp="1"
		;;
		*)
			rp=$((goal - 1))
		;;
	esac
	echo "${rp}"
}

function minimum_number_of_parts() {
	local goal="${1}"
	local total_number_of_parts="$(goal_to_part_count ${goal})"
	local redundant_part_count="$(redundant_parts ${goal})"
	echo "$((total_number_of_parts - redundant_part_count))"
}

function check_one_file_part_coverage_impl_() {
	local path="${1}"
	local expected_number_of_parts="${2}"
	local size=$(size_of "${path}")
	local n_chunks="$(filesize_to_chunk_count ${size})"
	local goal="$(saunafs getgoal "${path}" | awk '{print $2}')"
	local fileinfo="$(saunafs fileinfo ${path})"
	# echo "DEBUG: ${FUNCNAME[0]} pwd=$(pwd), path=${path}, n_chunks=${n_chunks}, goal=${goal}, expected_number_of_parts=${expected_number_of_parts}"
	if [[ "${goal}" =~ ^(xor|ec) ]] ; then
		for n in $(seq ${n_chunks}); do
			local unique_parts="$(echo "${fileinfo}" | sed -n "/chunk $((n - 1))\\>/,/chunk ${n}\\>/p" | awk '/copy/{print $5}' | sort -u | wc -l)"
			[[ "${unique_parts}" != "${expected_number_of_parts}" ]] && return 1
		done
	else
		for n in $(seq ${n_chunks}); do
			local copies="$(echo "${fileinfo}" | sed -n "/chunk $((n - 1))\\>/,/chunk ${n}\\>/p" | grep -c copy)"
			[[ "${copies}" != "${expected_number_of_parts}" ]] && return 1
		done
	fi
	return 0
}

function check_one_file_part_coverage() {
	local path="${1}"
	local expected_number_of_parts="${2}"
	local replication_timeout="${3}"
	assert_eventually 'check_one_file_part_coverage_impl_ "${path}" "${expected_number_of_parts}"' "${replication_timeout}"
}

function check_one_file_replicated() {
	local path="${1}"
	local replication_timeout="${2}"
	local goal="$(saunafs getgoal "${path}" | awk '{print $2}')"
	local expected_number_of_parts=$(goal_to_part_count ${goal})
	assert_eventually 'check_one_file_part_coverage_impl_ "${path}" "${expected_number_of_parts}"' "${replication_timeout}"
}


# Generation the server at port $1 answers with: zero before the first measurement, empty when it
# prints no measurement row or the command failed, so it is read only inside a wait that retries.
function chunk_health_measurement_generation_() {
	saunafs-admin chunks-health --porcelain localhost "${1}" \
		| awk '/^MEA /{print $2}'
}

# Waits until the report at port $1 covers the cluster as it is now, on a backend that measures in
# the background; elsewhere it returns at once. A server that prints no measurement row keeps its
# counters current, and a measuring server always answers the measured request, so an absent row
# is its own statement. Two measurements, not one: one already in flight may describe the cluster
# before the caller changed it.
function wait_for_chunk_health_measurement() {
	local port="${1}"
	local timeout="${2:-}"
	[[ "${METADATA_BACKEND:-}" == "FDB" ]] || return 0
	# The no-row decision needs the command's own status: the harness runs without pipefail, so
	# a pipeline into awk would turn a failed command into an empty, successful answer.
	local report
	report="$(saunafs-admin chunks-health --porcelain localhost "${port}")" \
		|| test_fail "saunafs-admin chunks-health failed against port ${port}"
	local generation
	generation="$(awk '/^MEA /{print $2}' <<< "${report}")"
	[[ -n "${generation}" ]] || return 0

	# Only the value read with the command's status is trusted; a failed read inside the retry is
	# empty, fails the comparison and is retried.
	assert_eventually \
		'test "$(chunk_health_measurement_generation_ "${port}")" -ge "$((generation + 2))"' \
		"${timeout}"
}

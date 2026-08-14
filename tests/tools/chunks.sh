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

# count_registered_parts <path>
# Prints how many chunk parts the master currently has registered for a file,
# summed over all of its chunks. Counts one per copy for replicated goals and
# one per part for xor/ec goals, so a single-chunk ec(2,1) file reports 3 when
# fully registered.
#
# Useful while chunkservers are (re-)registering, when the cluster-wide
# counters cannot answer the question: "Chunk copies" counts chunks rather than
# parts, and a chunkserver's reported chunk count arrives in the space report it
# sends before registering anything. fileinfo is answered through the
# chunks-info path, which has no deferring logic, so reading it does not perturb
# what a test is measuring.
#
# Prints 0 rather than failing when the file has no registered part at all.
function count_registered_parts() {
	local path="${1}"
	saunafs fileinfo "${path}" 2>/dev/null | awk '$1 == "copy" { parts++ } END { print parts + 0 }'
}


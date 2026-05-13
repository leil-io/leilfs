# Clears page cache
drop_caches() {
	sudo sh -c 'echo 1 > /proc/sys/vm/drop_caches'
}

is_program_installed() {
	return $(which "$1" &>/dev/null)
}

system_init() {
	ulimit -n 10000
}

inode_of() {
	local path="${1}"
	stat --format=%i "${path}"
}

size_of() {
	local path="${1}"
	stat --format=%s "${path}"
}

get_nproc_clamped_between() {
	local minimum=$1
	local maximum=$2
	local procs_num=$(nproc)
	echo $(( (procs_num < minimum) ? minimum : (maximum < procs_num) ? maximum : procs_num ))
}

find_program() {
	local program_name=$1
	local program_path

	if program_path=$(command -v "${program_name}" 2>/dev/null); then
		echo "${program_path}"
		return 0
	fi

	for program_path in /usr/sbin /usr/bin /sbin /bin; do
		if [[ -x "${program_path}/${program_name}" ]]; then
			echo "${program_path}/${program_name}"
			return 0
		fi
	done

	return 1
}

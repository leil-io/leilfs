# Verify that Ganesha with the SaunaFS FSAL negotiates NFSv4 sessions for both
# explicit NFSv4.1 and NFSv4.2 mounts. Each mount performs basic file I/O and
# checks mountstats for the session handshake and reported sessions capability.

CHUNKSERVERS=3 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

mountpoint_path="${TEMP_DIR}/mnt/ganesha"
ganesha_config="${info[mount0]}/ganesha.conf"
ganesha_log="${TEMP_DIR}/ganesha-nfsv4-sessions.log"

test_error_cleanup() {
	set +e
	if mountpoint -q "${mountpoint_path}"; then
		sudo umount -l "${mountpoint_path}"
	fi
	sudo pkill -9 ganesha.nfsd
}
trap test_error_cleanup EXIT

get_mountstats_block() {
	local mountpoint=$1
	awk -v mountpoint="${mountpoint}" '
		$1 == "device" && index($0, " mounted on " mountpoint " ") { found = 1 }
		found { print }
		found && $0 == "" { exit }
	' /proc/self/mountstats
}

assert_mountstats_counter_positive() {
	local mountstats_block=$1
	local operation_name=$2
	local operation_count

	operation_count=$(awk -v operation="${operation_name}:" '$1 == operation { print $2 }' \
		<<< "${mountstats_block}")
	if ! [[ ${operation_count} =~ ^[0-9]+$ ]] || (( operation_count == 0 )); then
		test_fail "Expected positive ${operation_name} counter in mountstats:${mountstats_block}"
	fi
}

assert_mountstats_nfs4_feature() {
	local mountstats_block=$1
	local feature_name=$2

	if ! awk -v feature="${feature_name}" '
		/^[[:space:]]*nfsv4:/ {
			split($0, fields, /[[:space:],]+/)
			for (i in fields) {
				if (fields[i] == feature) {
					found = 1
				}
			}
		}
		END { exit found ? 0 : 1 }
	' <<< "${mountstats_block}"; then
		test_fail "Expected ${feature_name} in nfsv4 mountstats:${mountstats_block}"
	fi
}

assert_mountstats_nfs_version() {
	local mountstats_block=$1
	local nfs_version=$2
	local minor_version=${nfs_version#4.}

	if ! grep -Eq "(^|,)vers=4[.]${minor_version}(,|$)|(^|,)minorversion=${minor_version}(,|$)" \
		<<< "${mountstats_block}"; then
		test_fail "Expected an explicit NFSv${nfs_version} mount in mountstats:${mountstats_block}"
	fi
}

run_session_mount() {
	local nfs_version=$1
	local version_tag="nfsv${nfs_version/./}"
	local test_file="${mountpoint_path}/${version_tag}-session.txt"
	local renamed_file="${mountpoint_path}/${version_tag}-session-renamed.txt"
	local expected_content="${version_tag}-session-test"
	local mountstats_block

	sudo mount -vvvv -t nfs4 -o "nfsvers=${nfs_version},proto=tcp,sec=sys" \
		localhost:/ "${mountpoint_path}"

	echo "${expected_content}" > "${test_file}"
	assert_equals "${expected_content}" "$(cat "${test_file}")"
	mv "${test_file}" "${renamed_file}"
	assert_success test -f "${renamed_file}"

	mountstats_block=$(get_mountstats_block "${mountpoint_path}")
	if [[ -z ${mountstats_block} ]]; then
		test_fail "Could not find mountstats for ${mountpoint_path}"
	fi

	assert_mountstats_nfs_version "${mountstats_block}" "${nfs_version}"
	assert_mountstats_counter_positive "${mountstats_block}" "EXCHANGE_ID"
	assert_mountstats_counter_positive "${mountstats_block}" "CREATE_SESSION"
	assert_mountstats_nfs4_feature "${mountstats_block}" "sessions"

	sudo umount "${mountpoint_path}"
}

mkdir -p "${mountpoint_path}"
create_ganesha_pid_file

cat <<EOF > "${ganesha_config}"
NFSV4 {
	Grace_Period = 5;
	Lease_Lifetime = 5;
}
EXPORT
{
	Attr_Expiration_Time = 0;
	Export_Id = 99;
	Path = /;
	Pseudo = /;
	Access_Type = RW;
	Transports = TCP;
	FSAL {
		Name = SaunaFS;
		hostname = localhost;
		port = ${saunafs_info_[matocl]};
		io_retries = 5;
		cache_expiration_time_ms = 2500;
	}
	Protocols = 4;
	CLIENT {
		Clients = localhost;
	}
}
EOF

sudo /usr/bin/ganesha.nfsd -f "${ganesha_config}" -L "${ganesha_log}"

check_rpc_service
for nfs_version in 4.1 4.2; do
	run_session_mount "${nfs_version}"
done

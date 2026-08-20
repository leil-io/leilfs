# Older (unsupported) versions of SaunaFS used in tests and sources of its packages
#
# TODO: Rename the functions to something better, like *saunafs_old
# TODO: Don't use build information in debian package versions. It causes
# problems when downgrading, because you need to include the build information
# in the downgrade command.

SAUNAFSXX_TAG="4.1.0"
: "${APT_AUTH_LOCATION:="/etc/apt/auth.conf.d/saunafs.conf"}"

install_saunafsXX() {
	if [ "${SAUNAFSXX_DIR:?}" == "/" ]; then
		test_fail "Cowardly refusing to perform potentially destructive operation on /"
	fi
	rm -rf "${SAUNAFSXX_DIR:?}"
	mkdir -p "${SAUNAFSXX_DIR}"
	local distro="$(lsb_release -si)"
	case "${distro}" in
	Ubuntu | Debian)
		local distro_id="$(lsb_release -si | tr '[:upper:]' '[:lower:]' | tail -1)"
		local codename="$(lsb_release -sc | tail -1)"
		local release="$(lsb_release -sr | tail -1)"
		mkdir -p "${TEMP_DIR}/apt/apt.conf.d"
		mkdir -p "${TEMP_DIR}/apt/auth.conf.d"
		mkdir -p "${TEMP_DIR}/apt/sources.list.d"
		mkdir -p "${TEMP_DIR}/apt/var/cache/apt/archives/partial"
		mkdir -p "${TEMP_DIR}/apt/var/lib/apt/partial"
		mkdir -p "${TEMP_DIR}/apt/var/lib/dpkg"
		mkdir -p "${TEMP_DIR}/usr/share/keyrings"
		cp /var/lib/dpkg/status "${TEMP_DIR}/apt/var/lib/dpkg/status"
		if [ -f $APT_AUTH_LOCATION ]; then
			cp $APT_AUTH_LOCATION "${TEMP_DIR}/apt/auth.conf.d/saunafs.conf"
		fi
		cat >"${TEMP_DIR}/apt/apt.conf" <<END
Dir::Cache "${TEMP_DIR}/apt/var/cache/apt";
Dir::Etc::Parts "${TEMP_DIR}/apt/apt.conf.d";
Dir::Etc::netrc "${TEMP_DIR}/apt/auth.conf";
Dir::Etc::netrcparts "${TEMP_DIR}/apt/auth.conf.d";
Dir::Etc::SourceList "${TEMP_DIR}/apt/saunafs.list";
Dir::Etc::SourceParts "${TEMP_DIR}/apt/sources.list.d";
Dir::State "${TEMP_DIR}/apt/var/lib/apt";
Dir::State::status "${TEMP_DIR}/apt/var/lib/dpkg/status";
END
		curl -sSL "https://keyserver.ubuntu.com/pks/lookup?op=get&search=0xa80b96e2c79457d4" | \
			gpg --dearmor -o "${TEMP_DIR}/apt/saunafs-archive-keyring.gpg"
		local destdir="${TEMP_DIR}/apt/var/cache/apt/archives"
		echo "deb [arch=amd64 signed-by=${TEMP_DIR}/apt/saunafs-archive-keyring.gpg] https://repo.saunafs.com/repository/saunafs-${distro_id}-${release}/ ${codename} main" >"${TEMP_DIR}/apt/saunafs.list"
		env APT_CONFIG="${TEMP_DIR}/apt/apt.conf" apt-get update
		env APT_CONFIG="${TEMP_DIR}/apt/apt.conf" apt-get install --yes libyaml-cpp*
		# A test may pre-set SAUNAFSXX_TAG_APT (together with SAUNAFSXX_TAG) to pin a
		# different released version; newer releases use plain <version>-<rev> strings.
		if [ -z "${SAUNAFSXX_TAG_APT:-}" ]; then
			SAUNAFSXX_TAG_APT="4.1.0-20240509-152518-stable-main-a7cb5669"
			if [ "${distro}" == Ubuntu ]; then
				case "${release}" in
					'22.04')
						SAUNAFSXX_TAG_APT="4.1.0-20240509-152513-stable-main-a7cb5669"
					;;
					'24.04')
						SAUNAFSXX_TAG_APT="4.1.0-20240509-152518-stable-main-a7cb5669"
					;;
					*)
						test_fail "Your Ubuntu release (${release}) is not supported."
					;;
				esac
			fi
		fi
		env APT_CONFIG="${TEMP_DIR}/apt/apt.conf" apt-get --yes --allow-downgrades install --download-only \
			saunafs-master="${SAUNAFSXX_TAG_APT}" \
			saunafs-chunkserver="${SAUNAFSXX_TAG_APT}" \
			saunafs-client="${SAUNAFSXX_TAG_APT}" \
			saunafs-adm="${SAUNAFSXX_TAG_APT}"
			# Not used right now
			# saunafs-metalogger="${SAUNAFSXX_TAG_APT}"
		# unpack binaries
		(	cd "${destdir}" || return 1
			# Whole directories, not single members: since 5.x the sfs* names are symlinks
			# to the leil-* binaries next to them, and extracting just the symlink member
			# would leave it dangling.
			find . -name "*master*.deb" -print0 | xargs -0 dpkg-deb --fsys-tarfile | tar -x ./usr/sbin/
			find . -name "*chunkserver*.deb" -print0 | xargs -0 dpkg-deb --fsys-tarfile | tar -x ./usr/sbin/
			find . -name "*client*.deb" -print0 | xargs -0 dpkg-deb --fsys-tarfile | tar -x ./usr/bin/
			find . -name "*adm*.deb" -print0 | xargs -0 dpkg-deb --fsys-tarfile | tar -x ./usr/bin/
			# find . -name "*metalogger*.deb" -print0 | xargs -0 dpkg-deb --fsys-tarfile | tar -x ./usr/sbin/
			cp -Rp usr/ "${SAUNAFSXX_DIR_BASE}/install"
		)
		;; # end of Ubuntu | Debian case
	CentOS | Fedora)
		local destdir="${TEMP_DIR}/saunafsxx_packages"
		mkdir "${destdir}"
		local url=""
		if [ "${distro}" == CentOS ]; then
			url="http://dev.saunafs.com/packages/centos.saunafs.repo"
		else
			url="http://dev.saunafs.com/packages/fedora.saunafs.repo"
		fi
		mkdir -p "${TEMP_DIR}/dnf/etc/yum.repos.d"
		cat >"${TEMP_DIR}/dnf/dnf.conf" <<END
[main]
logdir="${TEMP_DIR}/dnf/var/log"
cachedir="${TEMP_DIR}/dnf/var/cache"
persistdir="${TEMP_DIR}/dnf/var/lib/dnf"
reposdir="${TEMP_DIR}/dnf/etc/yum.repos.d"
END
		wget "${url}" -O "${TEMP_DIR}/dnf/etc/yum.repos.d/saunafs.repo"
		for pkg in {saunafs-master,saunafs-chunkserver,saunafs-client}-${SAUNAFSXX_TAG}; do
			fakeroot dnf -y --config="${TEMP_DIR}/dnf/dnf.conf" --destdir="${destdir}" download "${pkg}"
		done
		# unpack binaries
		(	cd "${destdir}"
		find . -name "*master*.rpm" -print0 | xargs -0 rpm2cpio | cpio -idm ./usr/sbin/sfsmaster
		find . -name "*chunkserver*.rpm" -print0 | xargs -0 rpm2cpio | cpio -idm ./usr/sbin/sfschunkserver
		find . -name "*client*.rpm" -print0 | xargs -0 rpm2cpio | cpio -idm ./usr/bin/*
		)
		;; # end of CentOS | Fedora case
	*)
		test_fail "Your distribution (${distro}) is not supported."
		;;
	esac
	test_saunafsXX_executables
	echo "Legacy SaunaFS packages installed."
}

test_saunafsXX_executables() {
	local awk_version_pattern="/${SAUNAFSXX_TAG//./\\.}/"
	test -x "${SAUNAFSXX_DIR}/bin/sfsmount"
	test -x "${SAUNAFSXX_DIR}/sbin/sfschunkserver"
	test -x "${SAUNAFSXX_DIR}/sbin/sfsmaster"
	"${SAUNAFSXX_DIR}/bin/sfsmount" --version 2>&1
	"${SAUNAFSXX_DIR}/sbin/sfschunkserver" -v
	"${SAUNAFSXX_DIR}/sbin/sfsmaster" -v
	assert_awk_finds "${awk_version_pattern}" "$("${SAUNAFSXX_DIR}/bin/sfsmount" --version 2>&1)"
	assert_awk_finds "${awk_version_pattern}" "$("${SAUNAFSXX_DIR}/sbin/sfschunkserver" -v)"
	assert_awk_finds "${awk_version_pattern}" "$("${SAUNAFSXX_DIR}/sbin/sfsmaster" -v)"
}

saunafsXX_chunkserver_daemon() {
	# shellcheck disable=SC2154
	"${SAUNAFSXX_DIR}/sbin/sfschunkserver" -c "${saunafs_info_["chunkserver${1}_cfg"]}" "${2}" | cat
	return "${PIPESTATUS[0]}"
}

saunafsXX_master_daemon() {
	"${SAUNAFSXX_DIR}/sbin/sfsmaster" -c "${saunafs_info_[master_cfg]}" "${1}" | cat
	return "${PIPESTATUS[0]}"
}

saunafsXX_shadow_daemon_n() {
	local id="${1}"
	shift
	if is_windows_system; then
		windows_server_aux "${SAUNAFSXX_DIR}/sbin/sfsmaster -c ${saunafs_info_[master${id}_cfg]}" "${@}"
	else
		"${SAUNAFSXX_DIR}/sbin/sfsmaster" -c "${saunafs_info_[master${id}_cfg]}" "${@}" | cat
	fi
	return "${PIPESTATUS[0]}"
}

# A generic function to run SaunaFS commands.
#
# Usage examples:
#   saunafs setgoal 3 file
#   saunafs dirinfo file
#   sfs sfsmetalogger stop
saunafsXX() {
	local command="${1}"
	shift
	for path in "${SAUNAFSXX_DIR}/bin" "${SAUNAFSXX_DIR}/sbin"; do
		if [ -x "${path}/${command}" ]; then
			command="${path}/${command}"
			break
		fi
	done
	"${command}" "${@}"
	return "${PIPESTATUS[0]}"
}

assert_saunafsXX_services_count_equals() {
	local mas_expected="${1}"
	local cs_expected="${2}"
	local cli_expected="${3}"
	assert_equals "${mas_expected}" "$(saunafs_old_admin_master info | grep -c "${SAUNAFSXX_TAG}")"
	assert_equals "${cs_expected}" "$(saunafs_old_admin_master list-chunkservers | grep -c "${SAUNAFSXX_TAG}")"
	assert_equals "${cli_expected}" "$(saunafs_old_admin_master list-mounts | grep -c "${SAUNAFSXX_TAG}")"
}

assert_no_saunafsXX_services_active() {
	assert_equals 0 "$(saunafs_admin_master info | grep "${SAUNAFSXX_TAG}" | wc -l)"
	assert_equals 0 "$(saunafs_admin_master list-chunkservers | grep "${SAUNAFSXX_TAG}" | wc -l)"
	assert_equals 0 "$(saunafs_admin_master list-mounts | grep "${SAUNAFSXX_TAG}" | wc -l)"
}

# TODO: Add metalogger and other service support

is_windows_system() {
	return 1
	grep /proc/version -e [Mm]icrosoft > /dev/null
}

get_windows_homepath() {
	local win_home_directory=$(/mnt/c/Windows/System32/cmd.exe /C "echo %homepath%" | tr -d "\r\n")
	echo -n "/mnt/c${win_home_directory//\\//}"
}

get_windows_domain() {
	local win_domain=$(/mnt/c/Windows/System32/cmd.exe /C "echo %USERDOMAIN%" | tr -d "\r\n")
	echo -n "$win_domain"
}

# Load config file with machine-specific configuration
if [[ ! -z "${SAUNAFS_TESTS_CONF:-}" && -f "${SAUNAFS_TESTS_CONF}" ]]; then
	echo "Using \"${SAUNAFS_TESTS_CONF}\" tests configuration file"
	. "${SAUNAFS_TESTS_CONF}"
elif [[ -f /home/${SUDO_USER}/etc/saunafs/tests.conf ]]; then
	echo "Using \"/home/${SUDO_USER}/etc/saunafs/tests.conf\" tests configuration file"
	. /home/${SUDO_USER}/etc/saunafs/tests.conf
elif [[ -f /etc/saunafs_tests.conf ]]; then
	echo "Using the default \"/etc/saunafs_tests.conf\" tests configuration file"
	. /etc/saunafs_tests.conf
fi

# Set up the default configuration values if not set yet
# This is a list of all configuration variables, that these tests use
: ${SAUNAFS_DISKS:=}
: ${SAUNAFS_LOOP_DISKS:=}
: ${TEMP_DIR:=/tmp/SaunaFS-autotests}
: ${LEGACY_DIR:=/tmp/SaunaFS-autotests-legacy}
: ${SAUNAFSXX_DIR_BASE:=/tmp/SaunaFS-autotests-old}
: ${SAUNAFS_ROOT:=/usr/local}
: ${FIRST_PORT_TO_USE:=9600}
: ${ERROR_FILE:=}
: ${RAMDISK_DIR:=/mnt/ramdisk}
: ${TEST_OUTPUT_DIR:=$TEMP_DIR}
: ${USE_VALGRIND:=}
: ${DEBUG:=}
: ${DEBUG_LEVEL:=0}

# This has to be an absolute path!
TEMP_DIR=$(readlink -m "$TEMP_DIR")
mkdir -p "$TEMP_DIR"
chmod 777 "$TEMP_DIR"

# Prepare important environment variables
export PATH="$SAUNAFS_ROOT/sbin:$SAUNAFS_ROOT/bin:$PATH"
if is_windows_system; then
	export PATH="$(get_windows_homepath)/LeilFS:/mnt/c/Windows/System32:$PATH"
	export SAFS_MOUNT_COMMAND="sfsmount.exe"
	export SAFS_ADMIN_COMMAND="saunafs-admin.exe"
	export SAFS_SAUNAFS_COMMAND="saunafs.exe"
fi

# Quick checks needed to call test_begin and test_fail
if ((BASH_VERSINFO[0] * 100 + BASH_VERSINFO[1] < 402)); then
	echo "Error: bash v4.2 or newer required, but $BASH_VERSION found" >&2
	exit 1
fi
if ! touch "$TEMP_DIR/check_tmp_dir" || ! rm "$TEMP_DIR/check_tmp_dir"; then
	echo "Configuration error: cannot create files in $TEMP_DIR" >&2
	exit 1
fi

# This function should be called just after test_fail is able to work
check_configuration() {
	for prog in \
		$SAUNAFS_ROOT/sbin/{sfsmaster,sfschunkserver} \
		$SAUNAFS_ROOT/bin/saunafs \
		$SAUNAFS_ROOT/bin/file-generate \
		$SAUNAFS_ROOT/bin/file-validate; do
		if ! [[ -x $prog ]]; then
			test_fail "Configuration error, executable $prog not found"
		fi
	done

	if [[ ! -x $SAUNAFS_ROOT/bin/sfsmount ]]; then
		test_fail "Configuration error, sfsmount executable not found"
	fi

	if ! df -T "$RAMDISK_DIR" | grep "tmpfs\|ramfs" >/dev/null; then
		test_fail "Configuration error, ramdisk ($RAMDISK_DIR) is missing"
	fi

	for dir in "$TEMP_DIR" "$RAMDISK_DIR" "$TEST_OUTPUT_DIR" $SAUNAFS_LOOP_DISKS; do
		if [[ ! -w $dir ]]; then
			test_fail "Configuration error, cannot create files in $dir"
		fi
	done

	if ! cat /etc/fuse.conf >/dev/null; then
		test_fail "Configuration error, user $(whoami) is not a member of the fuse group"
	fi

	if ! grep '[[:blank:]]*user_allow_other' /etc/fuse.conf >/dev/null; then
		test_fail "Configuration error, user_allow_other not enabled in /etc/fuse.conf"
	fi
}

parse_true() {
	local value="${1:-}"
	if [[ -z "${value}" ]]; then
		return 1
	fi
	case "${value,,}" in
	1 | true | t | yes | y | on) return 0 ;;
	*) return 1 ;;
	esac
}

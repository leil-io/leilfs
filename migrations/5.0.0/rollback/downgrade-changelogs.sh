#!/bin/bash
set -e

scriptname=$0

LOG_DIR="/var/log/saunafs/downgrade"
LOG_FILE="${LOG_DIR}/changelog.log"
MASTER_DATA_DIR="/var/lib/saunafs"
DEFAULT_BACKUP_DIR_PREFIX="/var/lib/saunafs.backup"

usage() {
	{
		echo "Downgrade SaunaFS changelogs script"
		echo "This script will downgrade the changelogs from version 5.0.0 to 4.X.X."
		echo ""
		echo "Usage:"
		echo "${scriptname} -h | --help"
		echo "    Displays this help message."
		echo "${scriptname} [-b <BACKUP_DIR] [-d <MASTER_DATA_DIR>]"
		echo "    Downgrades the SaunaFS changelogs to the specified version."
		echo "    Includes the following steps:"
		echo "        1. Create a backup of the current version to"
		echo "           $DEFAULT_BACKUP_DIR_PREFIX.\$(YYYY.MM.DD~HH.MM.SS)."
		echo "        2. Apply fixes to the installation."
		echo "    Master and metalogger must be stopped to run this script."
		echo "    The script will check for this."
		echo ""
		echo "    Logging is enabled by default and will be saved to $LOG_FILE."
		echo "    Options:"
		echo "       -d <MASTER_DATA_DIR>       The path to the master data directory. Default is $MASTER_DATA_DIR."
		echo "       -b <BACKUP_DIR>            Where to backup files (default is"
		echo "                                  $DEFAULT_BACKUP_DIR_PREFIX.\$(YYYY.MM.DD~HH.MM.SS)."
	} >&2
}

if [[ $1 == "-h" || $1 == "--help" ]]; then
	usage
	exit 0
fi

while getopts "d:b:" opt; do
	case ${opt} in
		d)
			MASTER_DATA_DIR="${OPTARG}"
			;;
		b)
			BACKUP_DIR="${OPTARG}"
			;;
		\?)
			echo "Invalid option: -${OPTARG}" >&2
			usage
			exit 1
			;;
		:)
			echo "Option -${OPTARG} requires an argument." >&2
			usage
			exit 1
			;;
	esac
done

if [ -z "$BACKUP_DIR" ]; then
	BACKUP_DIR="$DEFAULT_BACKUP_DIR_PREFIX.$(date +%Y.%m.%d~%H.%M.%S)"
fi

# Useful when log file is not yet setup
log_no_file_message() {
	echo "$(date '+%Y-%m-%d %H:%M:%S') - $1" >&2
}

log_message() {
	echo "$(date '+%Y-%m-%d %H:%M:%S') - $1" | tee -a ${LOG_FILE} >&2
}

setup_logs() {
	if ! mkdir -p "${LOG_DIR}"; then
		log_no_file_message "Could not create path to $LOG_DIR. Exiting..."
		exit 1
	fi

	if ! [ -f "${LOG_FILE}" ]; then
		if ! touch "${LOG_FILE}"; then
			log_no_file_message "Could not create $LOG_FILE. Exiting..." >&2
			exit 1
		fi
	elif ! [ -w "${LOG_FILE}" ]; then
		log_no_file_message "No permissions to write to $LOG_FILE. Exiting..." >&2
		exit 1
	fi
}

sanity_check() {
	if ! [ -d "$MASTER_DATA_DIR" ]; then
		log_message "The data directory $MASTER_DATA_DIR does not exist. Cannot continue."
		exit 1
	fi

}

create_initial_backup() {
	# Backup the current version binary and data directory
	if ! mkdir -p "${BACKUP_DIR}"; then
		log_message "Could not create ${BACKUP_DIR}, not continuing with conversion!"
		exit 1;
	fi

	if ! masterDataKB=$(du -sk "${MASTER_DATA_DIR}" 2>/dev/null | cut -f1); then
		log_message "Could not determine size of ${MASTER_DATA_DIR}. Not continuing conversion!"
		exit 1
	fi

	if ! backupRemainingSpaceKB=$(df -Pk "${BACKUP_DIR}" | tail -1 | awk '{print $4}' ); then
		log_message "Could not determine remaining size of ${BACKUP_DIR}. Not continuing conversion!"
		exit 1
	fi

	log_message "Size used by master: $masterDataKB KB"
	log_message "Size available for backup: $backupRemainingSpaceKB KB"

	if [ "$backupRemainingSpaceKB" -lt "${masterDataKB}" ]; then
		log_message "Not enough space for backup, not continuing with conversion!"
		exit 1;
	fi

	log_message "Enough space for backup."
	log_message "Creating backup of ${MASTER_DATA_DIR} to ${BACKUP_DIR}..."
	if ! cp -r "${MASTER_DATA_DIR}" "${BACKUP_DIR}"; then
		log_message "Could not copy ${MASTER_DATA_DIR} to ${BACKUP_DIR}, not continuing with conversion!"
		exit 1;
	fi
	log_message "Initial backup created at ${BACKUP_DIR}"
}

apply_fixes() {
	# Apply fixes to the installation
	log_message "Applying fixes to the installation..."
	# Fix the LENGTH function in changelog.sfs files
	find "${MASTER_DATA_DIR}" -type f -name "*changelog*sfs*" | while IFS= read -r file; do
		log_message "Editing $file with sed."
		if ! sed -i 's/LENGTH(\([^,]*,[^,]*\),[^)]*)/LENGTH(\1)/g' "${file}"; then
			log_message "sed failed in editing LENGTH in file, please restore from backup"
			exit 1;
		fi
	done
	log_message "Fixes applied successfully"
}

test_if_server_running() {
	server="$1"
	if pgrep -x "$server" >&2; then
		log_message "pgrep is reporting $server is running!"
		log_message "$server is still running, please stop $server before running this script"
		exit 1
	fi
}

test_if_servers_running() {
	test_if_server_running "sfsmaster"
	log_message "sfsmaster is not running"
	test_if_server_running "sfsmetalogger"
	log_message "sfsmetalogger is not running"
}

setup_logs
sanity_check
test_if_servers_running
create_initial_backup
apply_fixes
log_message "Downgrade process completed successfully"

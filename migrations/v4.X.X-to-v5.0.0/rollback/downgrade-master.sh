#!/bin/bash

scriptname=$0

usage() {
	{
		echo "Downgrade master server script"
		echo "This script will downgrade the master server from version 5.0.0 to 4.X.X."
		echo ""
		echo "Usage:"
		echo "${scriptname} -h | --help"
		echo "    Displays this help message."
		echo "${scriptname} -v <TARGET_VERSION> [-b <MASTER_BINARY_FILE>] [-d <MASTER_DATA_DIR>]"
		echo "    Downgrades the master server service to the specified version."
		echo "    Includes the following steps:"
		echo "        1. Stop the master server service."
		echo "        2. Create a backup of the current version to /tmp/downgrade-master/backup."
		echo "        3. Apply fixes to the installation."
		echo "        4. Downgrade the master server to the specified version."
		echo "        5. Start the master server service."
		echo ""
		echo "    Logging is enabled by default and will be saved to /tmp/downgrade-master/downgrade.log."
		echo "    Options:"
		echo "       -v <TARGET_VERSION>        The version to downgrade to. This is a required option. Example: 4.5.1-20241109-061218-stable-main-3aa2a609."
		echo "       -b <MASTER_BINARY_FILE>    The path to the master binary file. Default is /usr/sbin/sfsmaster."
		echo "       -d <MASTER_DATA_DIR>       The path to the master data directory. Default is /var/lib/saunafs."
	} >&2
	exit 0
}

if [ -z "$1" ]; then
	echo "Error: Missing required argument." >&2
	echo ""
	usage
fi

if [[ $1 == "-h" || $1 == "--help" ]]; then
	usage
fi

MASTER_BINARY_FILE=/usr/sbin/sfsmaster
MASTER_DATA_DIR=/var/lib/saunafs

while getopts "v:b:d:" opt; do
	case ${opt} in
		v)
			TARGET_VERSION=${OPTARG}
			;;
		b)
			MASTER_BINARY_FILE=${OPTARG}
			;;
		d)
			MASTER_DATA_DIR=${OPTARG}
			;;
		\?)
			echo "Invalid option: -${OPTARG}" >&2
			usage
			;;
		:)
			echo "Option -${OPTARG} requires an argument." >&2
			usage
			;;
	esac
done

if [ -z "${TARGET_VERSION}" ]; then
	echo "Error: Missing target version." >&2
	echo ""
    usage
fi

CURRENT_VERSION=$(${MASTER_BINARY_FILE} -v | awk 'NR==1 {print $2}')

mkdir -p /tmp/downgrade-master
BACKUP_DIR="/tmp/downgrade-master/backup"
LOG_FILE="/tmp/downgrade-master/downgrade.log"

log_message() {
	echo "$(date '+%Y-%m-%d %H:%M:%S') - $1" | tee -a ${LOG_FILE}
}

stop_master_server() {
	# Stop the master server
	if systemctl start saunafs-master; then
		log_message "Master stop command executed successfully"
	else
		log_message "Master stop command executed with errors"
		exit 1
	fi
}

create_backup() {
	# Backup the current version binary and data directory
	log_message "Creating backup of current version ${CURRENT_VERSION}..."
	mkdir -p ${BACKUP_DIR}
	cp ${MASTER_BINARY_FILE} ${BACKUP_DIR}
	cp -r ${MASTER_DATA_DIR} ${BACKUP_DIR}
	log_message "Backup created at ${BACKUP_DIR}"
}

apply_fixes() {
	# Apply fixes to the installation
	log_message "Applying fixes to the installation..."
	# Fix the LENGTH function in changelog.sfs files
	find "${MASTER_DATA_DIR}" -type f -name "changelog.sfs*" | while IFS= read -r file; do
		sed -i 's/LENGTH(\([^,]*,[^,]*\),[^)]*)/LENGTH(\1)/g' "${file}"
	done
	log_message "Fixes applied successfully"
}

downgrade_master_server() {
	# Downgrade the master server to the specified version
	log_message "Downgrading master server to version ${TARGET_VERSION}..."
	if apt install saunafs-master=${TARGET_VERSION}; then
		log_message "Master downgrade command executed successfully"
	else
		log_message "Master downgrade command executed with errors"
		exit 1
	fi
	log_message "Master server downgraded to version ${TARGET_VERSION}"
}

start_master_server() {
	# Start the master server
	log_message "Starting master server..."
	if systemctl start saunafs-master; then
		log_message "Master start command executed successfully"
	else
		log_message "Master start command executed with errors"
		exit 1
	fi
}

log_message "Starting downgrade process from version ${CURRENT_VERSION} to ${TARGET_VERSION}"
stop_master_server
create_backup
apply_fixes
downgrade_master_server
start_master_server
log_message "Downgrade process completed successfully"

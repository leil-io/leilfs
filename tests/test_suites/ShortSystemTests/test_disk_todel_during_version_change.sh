timeout_set '2 minutes'

# A reload's old-version to-delete report arrives while the master's copy is
# BUSY at v+1. Preserve that version without losing the disk's evacuation flag.
disk_state_report_order=busy
source "${SOURCE_DIR}/tests/tools/disk_state_version_race.sh"

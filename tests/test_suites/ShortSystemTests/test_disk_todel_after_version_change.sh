timeout_set '2 minutes'

# Deliver the reload's old-version to-delete report after the overwrite finishes.
# Reconciliation must also work when no operation-completion callback remains.
disk_state_report_order=settled
source "${SOURCE_DIR}/tests/tools/disk_state_version_race.sh"

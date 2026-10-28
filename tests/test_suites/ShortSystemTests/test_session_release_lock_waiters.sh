timeout_set 4 minutes

# Both cleanup triggers must install one exclusive waiter and retain the conflicting request.
sustain=$(timeout_rescale_seconds 60)
servers=2
if [[ "${METADATA_BACKEND:-}" == FDB ]]; then servers=1; fi
CHUNKSERVERS=1 \
	MASTERSERVERS="${servers}" \
	MOUNTS=4 \
	USE_RAMDISK=YES \
	MASTER_EXTRA_CONFIG="SESSION_SUSTAIN_TIME=${sustain}" \
	MOUNT_EXTRA_CONFIG="enablefilelocks=1|sfscachemode=NEVER" \
	setup_local_empty_saunafs info

source "${SOURCE_DIR}/tests/tools/session_lock_helpers.sh"
if [[ "${servers}" == 2 ]]; then assert_success saunafs_master_n 1 start; fi
session_lock_prepare
if [[ "${servers}" == 2 ]]; then session_lock_shadow_matches; fi
session_lock_release_holders
session_lock_check_release
if [[ "${servers}" == 2 ]]; then session_lock_shadow_matches; fi

# The other session must become the stored holder when the first winner unlocks.
for name in "${lock_cases[@]}"; do session_lock_unlock_first "${name}"; done
if [[ "${servers}" == 2 ]]; then session_lock_shadow_matches; fi
cleanup_session_locks

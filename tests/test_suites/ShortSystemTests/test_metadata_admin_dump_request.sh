#!/usr/bin/env bash
# test_admin_offbox_metadata_dump.sh
#
# Draft test for off-box admin save-metadata using an intent + staging workflow.
# Master:
#   - rotates changelog to freeze a cut (creating changelog.sfs.1),
#   - writes an intent file describing the cut (dump.job/<job-id> or dump.intent),
#   - waits for a staged metadata file that matches the cut,
#   - validates and atomically promotes it to metadata.sfs,
#   - only then reports success to the admin caller and broadcasts.
#
# External orchestrator (simulated here locally):
#   - watches for intent,
#   - runs sfsmetarestore on the cut,
#   - generates metadata.sfs.<Vcut+1>.<timestamp>.ok,
#   - pushes it to the master’s staging dir.

set -euo pipefail

# Test harness expects these utilities. Source common helpers if needed.
# shellcheck source=/dev/null
. "$(dirname "$0")/testlib.sh"

timeout_set 120 seconds

# TODO: Adjust config keys to match implementation:
# - SFS_OFFBOX_DUMP_ENABLED enables the off-box flow
# - SFS_OFFBOX_STAGING_DIR points to the directory where the external tool drops results
# - SFS_OFFBOX_INTENT_DIR is where the master writes job intent
# - SFS_OFFBOX_SYNC_TIMEOUT defines synchronous admin waiter timeout
STAGING_DIR="$TEMP_DIR/offbox_staging"
INTENT_DIR="$TEMP_DIR/offbox_intent"
mkdir -p "$STAGING_DIR" "$INTENT_DIR"

master_extra_config=""
master_extra_config+="SFS_OFFBOX_DUMP_ENABLED = 1"
master_extra_config+="|SFS_OFFBOX_STAGING_DIR = $STAGING_DIR"
master_extra_config+="|SFS_OFFBOX_INTENT_DIR = $INTENT_DIR"
master_extra_config+="|SFS_OFFBOX_SYNC_TIMEOUT = 30"
master_extra_config+="|BACK_META_KEEP_PREVIOUS = 5"
# Keep default BACK_LOGS policy (or set explicitly if your harness requires).

CHUNKSERVERS=3 \
  USE_RAMDISK=YES \
  MOUNT_EXTRA_CONFIG="sfscachemode=NEVER|enablefilelocks=1" \
  SFSEXPORTS_EXTRA_OPTIONS="allcanchangequota" \
  MASTER_EXTRA_CONFIG=$master_extra_config \
  setup_local_empty_saunafs info

# Convenience paths into the master data dir
MD="$(
  cd "${info[master_data_path]}" && pwd
)"
CHANGELOG="$MD/changelog.sfs"
METADATA="$MD/metadata.sfs"

# Helper: last cut version (tail of changelog.sfs.1)
function last_cut_version() {
  tail -n1 "$MD/changelog.sfs.1" | cut -d':' -f1 | tr -d ' '
}

# Helper: version in a metadata file
function metadata_version() {
  sfsmetadump "$1" | awk 'NR==2{print $6}'
}

# Simulated external orchestrator: watches INTENT_DIR and produces staged result
cat > "$TEMP_DIR/offbox_orchestrator.sh" << 'ORCH'
#!/usr/bin/env bash
set -euo pipefail

INTENT_DIR="$1"
STAGING_DIR="$2"
MASTER_DATA_DIR="$3"

# This loop ends after it processes exactly one intent (the test starts and stops it as needed)
while true; do
  # Wait up to 25s to detect an intent file; exit non-zero to simulate "not responding"
  for _ in $(seq 1 50); do
    intent="$(ls -1 "$INTENT_DIR" 2>/dev/null | head -n1 || true)"
    if [[ -n "${intent:-}" ]]; then break; fi
    sleep 0.5
  done
  [[ -n "${intent:-}" ]] || exit 1

  INTENT_PATH="$INTENT_DIR/$intent"
  # Example fields (adjust to your intent schema):
  # job_id=..., cut_version=..., cluster_id=...
  CUT_V="$(awk -F= '/^cut_version=/{print $2}' "$INTENT_PATH")"
  [[ -n "$CUT_V" ]]

  # Build staged metadata matching the cut using local sfsmetarestore
  # Assumes MASTER_DATA_DIR has metadata.sfs (old) and changelog.sfs.1 (cut)
  # You may need to pass flags to sfsmetarestore that match your environment.
  OUT_TMP="$STAGING_DIR/metadata.sfs.$((CUT_V+1)).tmp"
  OUT_OK="$STAGING_DIR/metadata.sfs.$((CUT_V+1)).$(date +%s).ok"

  # Reconstruct up to the cut into OUT_TMP, then rename to .ok
  sfsmetarestore -d "$MASTER_DATA_DIR" -o "$OUT_TMP"
  mv "$OUT_TMP" "$OUT_OK"

  # Optionally write a status for debugging
  echo "cut_version=$CUT_V" > "$STAGING_DIR/last_status"

  # Process one intent and exit
  exit 0
done
ORCH
chmod +x "$TEMP_DIR/offbox_orchestrator.sh"

backup_copies=1
function check_backup_copies() {
  expect_equals $backup_copies $(ls "$MD"/metadata.sfs.? | wc -l)
  expect_file_exists "$METADATA"
  for (( i = 1 ; i <= backup_copies ; ++i )); do
    expect_file_exists "$MD/metadata.sfs.$i"
  done
}

# Scenario 1: Happy path — admin save-metadata completes after orchestrator produces staged file
(
  cd "${info[mount0]}"

  # Generate some changes
  FILE_SIZE=200K file-generate a
  saunafs setgoal 3 a
  echo ok > b
)

# Start orchestrator in background to satisfy the intent
"$TEMP_DIR/offbox_orchestrator.sh" "$INTENT_DIR" "$STAGING_DIR" "$MD" &
orch_pid=$!

# Invoke admin save-metadata (synchronous)
assert_success saunafs_admin_master save-metadata

# Validate: .1 exists, and metadata.sfs version == last_cut + 1
assert_file_exists "$MD/changelog.sfs.1"
cut_v="$(last_cut_version)"
assert_success test -n "$cut_v"
expect_equals $((cut_v+1)) "$(metadata_version "$METADATA")"
if ((backup_copies < 5)); then backup_copies=$((backup_copies + 1)); fi
check_backup_copies

# Clean up the orchestrator if still alive
kill "$orch_pid" 2>/dev/null || true
wait "$orch_pid" 2>/dev/null || true

# Scenario 2: Timeout/failure — no orchestrator; admin call should fail, logs retained
rm -f "$STAGING_DIR"/* "$INTENT_DIR"/*

# Generate more changes
(
  cd "${info[mount0]}"
  echo foo >> b
  mkdir cdir && touch cdir/f1
)

# Do NOT start orchestrator; expect timeout
assert_failure saunafs_admin_master save-metadata

# Ensure the cut remains present and usable for recovery; metadata.sfs not advanced
assert_file_exists "$MD/changelog.sfs.1"
# The stored metadata version should be <= previous version; no promotion happened
prev_v="$(metadata_version "$METADATA")"
sleep 1
expect_equals "$prev_v" "$(metadata_version "$METADATA")"

# Scenario 3: Duplicate/late arrival — two staged files; first wins, late ignored
rm -f "$STAGING_DIR"/* "$INTENT_DIR"/*

# Generate more changes to create a new cut
(
  cd "${info[mount0]}"
  echo bar >> b
)

# Start a custom orchestrator that creates two results, second arrives later
cat > "$TEMP_DIR/offbox_orchestrator_dup.sh" << 'ORCH2'
#!/usr/bin/env bash
set -euo pipefail

INTENT_DIR="$1"; STAGING_DIR="$2"; MASTER_DATA_DIR="$3"

# Wait for intent
for _ in $(seq 1 50); do
  intent="$(ls -1 "$INTENT_DIR" 2>/dev/null | head -n1 || true)"
  [[ -n "${intent:-}" ]] && break
  sleep 0.5
done
[[ -n "${intent:-}" ]]

CUT_V="$(awk -F= '/^cut_version=/{print $2}' "$INTENT_DIR/$intent")"
OUT1="$STAGING_DIR/metadata.sfs.$((CUT_V+1)).$(date +%s).ok"
OUT2="$STAGING_DIR/metadata.sfs.$((CUT_V+1)).$(( $(date +%s)+1 )).ok"

# Produce first valid result
sfsmetarestore -d "$MASTER_DATA_DIR" -o "$STAGING_DIR/tmp1"
mv "$STAGING_DIR/tmp1" "$OUT1"

# Then produce a second (duplicate) result after a short delay
sleep 2
sfsmetarestore -d "$MASTER_DATA_DIR" -o "$STAGING_DIR/tmp2"
mv "$STAGING_DIR/tmp2" "$OUT2"
ORCH2
chmod +x "$TEMP_DIR/offbox_orchestrator_dup.sh"

"$TEMP_DIR/offbox_orchestrator_dup.sh" "$INTENT_DIR" "$STAGING_DIR" "$MD" &
orch_pid=$!

# The admin call should succeed once the first arrives; the late duplicate must be ignored
assert_success saunafs_admin_master save-metadata

# Validate version advancement matches the cut exactly once
assert_file_exists "$MD/changelog.sfs.1"
cut_v="$(last_cut_version)"
expect_equals $((cut_v+1)) "$(metadata_version "$METADATA")"
if ((backup_copies < 5)); then backup_copies=$((backup_copies + 1)); fi
check_backup_copies

kill "$orch_pid" 2>/dev/null || true
wait "$orch_pid" 2>/dev/null || true

# Scenario 4: Late arrival after timeout — master should ignore it
rm -f "$STAGING_DIR"/* "$INTENT_DIR"/*

# Generate another cut
(
  cd "${info[mount0]}"
  echo baz >> b
)

# Start a "too-late" producer: deliver result after master’s sync timeout
cat > "$TEMP_DIR/offbox_orchestrator_late.sh" << 'ORCH3'
#!/usr/bin/env bash
set -euo pipefail

INTENT_DIR="$1"; STAGING_DIR="$2"; MASTER_DATA_DIR="$3"

for _ in $(seq 1 50); do
  intent="$(ls -1 "$INTENT_DIR" 2>/dev/null | head -n1 || true)"
  [[ -n "${intent:-}" ]] && break
  sleep 0.5
done
[[ -n "${intent:-}" ]]

CUT_V="$(awk -F= '/^cut_version=/{print $2}' "$INTENT_DIR/$intent")"
sleep 40  # greater than SFS_OFFBOX_SYNC_TIMEOUT to force admin timeout

OUT="$STAGING_DIR/metadata.sfs.$((CUT_V+1)).$(date +%s).ok"
sfsmetarestore -d "$MASTER_DATA_DIR" -o "$STAGING_DIR/tmp"
mv "$STAGING_DIR/tmp" "$OUT"
ORCH3
chmod +x "$TEMP_DIR/offbox_orchestrator_late.sh"

"$TEMP_DIR/offbox_orchestrator_late.sh" "$INTENT_DIR" "$STAGING_DIR" "$MD" &
orch_pid=$!

assert_failure saunafs_admin_master save-metadata

# The late file might land now; ensure master did not promote it silently
prev_v="$(metadata_version "$METADATA")"
sleep 2
expect_equals "$prev_v" "$(metadata_version "$METADATA")"

kill "$orch_pid" 2>/dev/null || true
wait "$orch_pid" 2>/dev/null || true

echo "OK"

timeout_set 3 minutes

assert_program_installed setfacl getfacl

# Regression test: a master killed mid-burst and recovered by promoting its shadow must durably
# persist the changelog-replayed state, so data the promoted master serves is not lost on a later
# restart.
#
# Scenario:
#   1. The active master accepts a burst of writes. Each write is appended to the changelog
#      (synchronous on disk, streamed to the shadow immediately) but is not yet captured in a
#      saved metadata image.
#   2. The master is SIGKILLed right after the burst, before it can save -- so the tail of the
#      burst lives only in the changelog. The shadow already received those changelog entries.
#   3. The shadow is promoted. It replays the streamed changelog, so the live filesystem serves
#      every crash-window file.
#   4. The promoted master saves metadata and is restarted, reloading from the saved image. The
#      crash-window state the live filesystem served must still be present: a backend that fails
#      to persist the replayed state on promotion loses the tail of the burst on reload.
#
# The burst touches every metadata kind (file content/chunk, xattr, quota, ACL, and deletions),
# so the reload validates that promotion persists all of them, not just names.

# Disable periodic metadata dumping so the burst is not auto-saved before the crash; the shadow
# must recover it from the streamed changelog.
master_cfg="METADATA_DUMP_PERIOD_SECONDS = 0"

CHUNKSERVERS=1 \
	MASTERSERVERS=2 \
	USE_RAMDISK="YES" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER,sfsdirentrycacheto=0" \
	SFSEXPORTS_EXTRA_OPTIONS="allcanchangequota,ignoregid" \
	MASTER_EXTRA_CONFIG="$master_cfg" \
	setup_local_empty_saunafs info

# Baseline namespace, fully saved before we start racing the crash window.
cd "${info[mount0]}"
mkdir baseline_dir
touch baseline_dir/baseline_file{1..20}
cd
assert_success saunafs_admin_master save-metadata

# Shadow follows the master through the changelog stream.
saunafs_master_n 1 start
assert_eventually "saunafs_shadow_synchronized 1"

# Crash-window writes: create a burst and SIGKILL the master immediately, before it can persist
# the burst (a graceful stop would save it). The changelog (on disk + streamed to the shadow)
# carries the entries regardless. The burst also touches file content (a chunk), an xattr, a
# quota and an ACL, plus deletions, so the reload validates that promotion persists every kind.
cd "${info[mount0]}"
touch crash_file{1..1000}
echo "crash-window-payload" > crash_content_file              # allocates a chunk
setfattr -n user.crashattr -v crashval crash_content_file     # xattr
saunafs setquota -u 4242 1GB 2GB 10 20 .                      # quota (limits for uid 4242)
setfacl -m user:saunafstest:rwx crash_content_file            # ACL
rm baseline_dir/baseline_file{1..10}                          # deletions in the unsaved tail
cd
saunafs_master_daemon kill   # SIGKILL: no graceful metadata save

# Promote the shadow. It replays the streamed changelog and serves the crash_files.
saunafs_make_conf_for_master 1
saunafs_master_daemon reload
saunafs_wait_for_all_ready_chunkservers
# Promotion may drain a large burst; wait until the mount is responsive again.
wait_for 'ls "${info[mount0]}" >/dev/null 2>&1' '60 seconds'

# Live filesystem after promotion: the shadow recovered the crash-window files from the changelog.
cd "${info[mount0]}"
live_count=$(ls -1 crash_file* 2>/dev/null | wc -l)
quota_live=$(saunafs repquota -u 4242 .)
acl_live=$(getfacl --absolute-names crash_content_file)
cd
echo "crash_files present after promotion (live, via changelog replay): $live_count"
if (( live_count == 0 )); then
	test_fail "Shadow recovered no crash-window files; cannot demonstrate the persistence gap"
fi

# Save metadata on the promoted master, then restart it. The restart reloads the saved metadata
# image, so this checks the promoted master durably persisted the crash-window state.
assert_success saunafs_admin_master save-metadata
saunafs_master_daemon restart
saunafs_wait_for_all_ready_chunkservers
wait_for 'ls "${info[mount0]}" >/dev/null 2>&1' '60 seconds'

cd "${info[mount0]}"
reload_count=$(ls -1 crash_file* 2>/dev/null | wc -l)
cd
echo "crash_files present after reload: $reload_count"

# No data loss: every file the live filesystem served after promotion must survive the reload.
assert_equals "$live_count" "$reload_count"

# The chunk (file content), xattr, quota and ACL created in the crash window must also survive the
# reload. Each is compared against the live (post-promotion) value, so a kind the promotion forgets
# to persist diverges.
cd "${info[mount0]}"
assert_equals "crash-window-payload" "$(cat crash_content_file)"
assert_equals "crashval" "$(getfattr --absolute-names --only-values -n user.crashattr crash_content_file 2>/dev/null)"
assert_equals "$quota_live" "$(saunafs repquota -u 4242 .)"
assert_equals "$acl_live" "$(getfacl --absolute-names crash_content_file)"

# Deletion handling: 20 baseline files saved, 10 removed in the unsaved tail. The promotion must
# persist the deletions, so exactly 10 survive the reload (a re-add-only recovery would resurrect
# the deleted 10).
echo "baseline files after reload (expect 10): $(ls -1 baseline_dir/baseline_file* 2>/dev/null | wc -l)"
assert_equals 10 "$(ls -1 baseline_dir/baseline_file* 2>/dev/null | wc -l)"
cd

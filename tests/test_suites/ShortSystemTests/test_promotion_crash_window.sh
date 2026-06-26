timeout_set 3 minutes

assert_program_installed setfacl getfacl setfattr getfattr

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
#      crash-window state the live filesystem served must still be present. If promotion does not
#      make the replayed state durable, the tail of the burst is lost on reload.
#
# The burst touches every metadata kind (file content/chunk, xattr, quota, ACL, and deletions),
# so the reload validates that promotion persists all of them, not just names.

# Disable periodic metadata dumping and automatic FDB writer flushes. Explicit metadata saves still
# drain the writer, so the baseline below becomes durable while the later crash-window tail cannot
# reach FDB before the primary is killed.
master_cfg="METADATA_DUMP_PERIOD_SECONDS = 0|METADATA_FDB_DEBUG_DISABLE_PERIODIC_FLUSH = 1"

CHUNKSERVERS=1 \
	MASTERSERVERS=2 \
	MOUNTS=2 \
	USE_RAMDISK="YES" \
	MOUNT_0_EXTRA_CONFIG="sfscachemode=NEVER,sfsdirentrycacheto=0" \
	MOUNT_1_EXTRA_CONFIG="sfsmeta" \
	SFSEXPORTS_EXTRA_OPTIONS="allcanchangequota,ignoregid" \
	SFSEXPORTS_META_EXTRA_OPTIONS="nonrootmeta" \
	MASTER_EXTRA_CONFIG="$master_cfg" \
	MASTER_0_EXTRA_CONFIG="MAGIC_DEBUG_LOG = ${TEMP_DIR}/master0.log|LOG_FLUSH_ON=DEBUG" \
	setup_local_empty_saunafs info

# Confirm that the deterministic test gate is active when exercising the forkless backend. Manual
# save-metadata drains are intentionally unaffected by this option. The FILE backend has no async
# FDB writer and therefore does not emit this message.
if [[ "${info[metadata_backend]}" == "FORKLESS" ]]; then
	assert_eventually \
		"grep -q 'Periodic FDB metadata flush disabled' '${TEMP_DIR}/master0.log'"
fi

# Baseline namespace, fully saved before we start racing the crash window.
cd "${info[mount0]}"
mkdir baseline_dir
touch baseline_dir/baseline_file{1..20}

# Create baseline metadata that the crash-window tail will remove. Keep independent surviving
# files for xattr, ACL, and chunk removal so each final state remains directly observable after
# reload.
echo "delete-everything" > removed_node
saunafs settrashtime 0 removed_node
setfattr -n user.removed -v old removed_node
setfacl -m user:saunafstest:rwx removed_node

touch removed_xattr
setfattr -n user.removed -v old removed_xattr
touch removed_acl
setfacl -m user:saunafstest:rwx removed_acl
echo "removed-chunk" > removed_chunk
saunafs setquota -u 4343 1GB 2GB 10 20 .

# Put three files in trash and save that state so the crash window can exercise path replacement,
# permanent deletion, and recovery into the regular namespace independently.
touch detached_setpath detached_purge detached_undel
saunafs settrashtime 3600 detached_setpath detached_purge detached_undel
rm detached_setpath detached_purge detached_undel
cd
assert_success saunafs_admin_master save-metadata

# Shadow follows the master through the changelog stream.
saunafs_master_n 1 start
assert_eventually "saunafs_shadow_synchronized 1"

# Crash-window writes occur after the saved baseline and remain outside its metadata image until
# the shadow is promoted. The changelog (on disk and streamed to the shadow) carries the operations.
# The tail also touches file content, an xattr, a quota and an ACL, plus deletions, so the reload
# validates that promotion persists every kind.
cd "${info[mount0]}"
touch crash_file{1..100}
echo "crash-window-payload" > crash_content_file              # allocates a chunk
setfattr -n user.crashattr -v crashval crash_content_file     # xattr
saunafs setquota -u 4242 1GB 2GB 10 20 .                      # quota (limits for uid 4242)
setfacl -m user:saunafstest:rwx crash_content_file            # ACL
rm baseline_dir/baseline_file{1..10}

# Exercise actual removals for every independently persisted metadata section. removed_node has zero
# trash time, so deletion is permanent instead of moving the file into trash.
rm removed_node
setfattr -x user.removed removed_xattr
setfacl -b removed_acl
truncate -s 0 removed_chunk
saunafs setquota -u 4343 0 0 0 0 .
cd

# Exercise the changelog operations for changing trashed paths. SETPATH renames one file, PURGE
# permanently deletes another, and UNDEL restores the third into the regular namespace.
trash="${info[mount1]}/trash"
echo "renamed_detached" > "$trash"/*detached_setpath
assert_eventually "test -e '$trash'/*renamed_detached"
assert_success rm "$trash"/*detached_purge
assert_success mv "$trash"/*detached_undel "$trash"/undel/

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
removed_quota_live=$(saunafs repquota -u 4343 .)
removed_acl_live=$(getfacl --absolute-names removed_acl)
cd
echo "crash_files present after promotion (live, via changelog replay): $live_count"
assert_equals 100 "$live_count"
assert_file_not_exists "${info[mount0]}/removed_node"
assert_equals 0 "$(stat -c %s "${info[mount0]}/removed_chunk")"
assert_failure getfattr --absolute-names --only-values -n user.removed \
	"${info[mount0]}/removed_xattr"
assert_file_exists "${info[mount0]}/detached_undel"
assert_equals 1 "$(ls -1 "$trash"/*renamed_detached 2>/dev/null | wc -l)"
assert_equals 0 "$(ls -1 "$trash"/*detached_purge 2>/dev/null | wc -l)"

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

# Every removal observed immediately after promotion must survive the restart and reload.
assert_file_not_exists removed_node
assert_equals 0 "$(stat -c %s removed_chunk)"
assert_failure getfattr --absolute-names --only-values -n user.removed removed_xattr
assert_equals "$removed_acl_live" "$(getfacl --absolute-names removed_acl)"
assert_equals "$removed_quota_live" "$(saunafs repquota -u 4343 .)"
assert_file_exists detached_undel
assert_equals 1 "$(ls -1 "$trash"/*renamed_detached 2>/dev/null | wc -l)"
assert_equals 0 "$(ls -1 "$trash"/*detached_purge 2>/dev/null | wc -l)"

# Deletion handling: 20 baseline files saved, 10 removed in the unsaved tail. The promotion must
# persist the deletions, so exactly 10 survive the reload (a re-add-only recovery would resurrect
# the deleted 10).
echo "baseline files after reload (expect 10): $(ls -1 baseline_dir/baseline_file* 2>/dev/null | wc -l)"
assert_equals 10 "$(ls -1 baseline_dir/baseline_file* 2>/dev/null | wc -l)"
cd

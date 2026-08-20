timeout_set 5 minutes

# Both admin commands must stay compatible across a version boundary, in both directions:
# the current admin against the newest released master (which rejects the identity-bearing
# status request and kills that connection), and that release's own admin against a current
# master (which must keep answering the original packet shapes unchanged).
legacy_version="5.11.0"

CHUNKSERVERS=0 \
	MASTERSERVERS=2 \
	USE_RAMDISK=YES \
	START_WITH_LEGACY_SAUNAFS=YES \
	SAUNAFSXX_TAG="${legacy_version}" \
	SAUNAFSXX_TAG_APT="${legacy_version}-1" \
	setup_local_empty_saunafs info

# Explicit path, so every phase below is unambiguous about which admin binary it runs
# (the released one is always reached through saunafs_old_admin_master).
new_admin="${SAUNAFS_ROOT}/bin/saunafs-admin"

# Baseline: the cluster really runs the released version at this point. The version
# switch proven below would be vacuous if the build under test WERE the legacy release.
assert_success test "${SAUNAFS_VERSION}" != "${legacy_version}"
assert_equals 1 $(saunafs_admin_master info | grep "^SaunaFS v${legacy_version//./\\.}$" | wc -l)

# Current admin against the released master: the identity-bearing request is rejected there
# (that connection gets killed), so a correct answer proves the fresh-connection fallback
# to the original request shape works against a real released binary.
statusOldMaster=$("${new_admin}" metadataserver-status --porcelain localhost "${info[matocl]}")
assert_equals $'master\trunning' "$(echo "${statusOldMaster}" | cut -f1-2)"
assert_equals 3 "$(echo "${statusOldMaster}" | awk -F'\t' '{print NF}')"

# With a released shadow up, the list command's per-member follow-up must pick the
# original status shape from the member's own advertised version, with no extra query or fallback.
saunafsXX_shadow_daemon_n 1 start
assert_eventually "saunafs_shadow_synchronized 1"

listOldMaster=$("${new_admin}" list-metadataservers --porcelain localhost "${info[matocl]}")
assert_equals 2 "$(echo "${listOldMaster}" | wc -l)"
assert_awk_finds '/ master running /' "${listOldMaster}"
assert_awk_finds '/ shadow connected /' "${listOldMaster}"
assert_awk_finds "/${legacy_version//./\\.}/" "${listOldMaster}"

# Upgrade master and shadow in place to the current build, and prove the switch really
# happened before asserting anything against "the new master".
saunafs_master_daemon restart
saunafs_master_n 1 restart
assert_eventually "saunafs_shadow_synchronized 1"
assert_equals 1 $(saunafs_admin_master info | grep "^SaunaFS v${SAUNAFS_VERSION//./\\.}$" | wc -l)

# The released admin against the current master and shadow: both commands must keep
# answering in the original shapes, unchanged.
statusOldAdmin=$(saunafs_old_admin_master metadataserver-status --porcelain)
assert_equals $'master\trunning' "$(echo "${statusOldAdmin}" | cut -f1-2)"
assert_equals 3 "$(echo "${statusOldAdmin}" | awk -F'\t' '{print NF}')"

listOldAdmin=$(saunafs_old_admin_master list-metadataservers --porcelain)
assert_equals 2 "$(echo "${listOldAdmin}" | wc -l)"
assert_awk_finds '/ master running /' "${listOldAdmin}"
assert_awk_finds '/ shadow connected /' "${listOldAdmin}"
# Both members advertise the current build's version (the porcelain list's last column)
# in the released admin's own output: the upgrade replaced the shadow too, not only the
# master the info check above covers.
assert_equals 2 "$(echo "${listOldAdmin}" | awk -v v="${SAUNAFS_VERSION}" '$7 == v' | wc -l)"

# The current admin against the current master, in the same cluster, for symmetry: the
# porcelain status must stay the original three fields (no identity outside a cluster).
statusNewMaster=$("${new_admin}" metadataserver-status --porcelain localhost "${info[matocl]}")
assert_equals $'master\trunning' "$(echo "${statusNewMaster}" | cut -f1-2)"
assert_equals 3 "$(echo "${statusNewMaster}" | awk -F'\t' '{print NF}')"

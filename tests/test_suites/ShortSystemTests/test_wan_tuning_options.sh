# Covers the mount options added for high-latency links: sfsxattrs, sfsacl, the
# sfswan preset that combines them, and sfschunkserverlatencysort.

assert_program_installed setfacl getfacl setfattr getfattr

# Attribute names of a file, one per line, sorted; empty if there are none. Fails
# if the listing does, so a refused listing cannot read as an empty one.
xattr_names() {
	local listing
	listing=$(getfattr -m - --absolute-names "$1") || return
	printf '%s\n' "$listing" | sed -e '/^#/d' -e '/^$/d' | sort
}

# Value of a mount option as the mount reports it in .saunafs_mount_info.
mount_option() {
	awk -v name="$1:" '$1 == name { print $2 }' "$2/.saunafs_mount_info"
}

# One mount per option, plus a stock one to compare them against.
CHUNKSERVERS=3 \
	MOUNTS=7 \
	USE_RAMDISK=YES \
	SFSEXPORTS_EXTRA_OPTIONS=nomasterpermcheck,ignoregid \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	MOUNT_0_EXTRA_CONFIG="sfswan" \
	MOUNT_1_EXTRA_CONFIG="sfschunkserverlatencysort=1" \
	MOUNT_2_EXTRA_CONFIG="sfsacl=0" \
	MOUNT_3_EXTRA_CONFIG="sfsxattrs=0" \
	MOUNT_4_EXTRA_CONFIG="sfswan=0" \
	MOUNT_6_EXTRA_CONFIG="sfswan|sfsacl=1|sfsxattrs=1" \
	setup_local_empty_saunafs info

wan="${info[mount0]}"
latency_sorted="${info[mount1]}"
no_acls="${info[mount2]}"
no_xattrs="${info[mount3]}"
wan_disabled="${info[mount4]}"
default="${info[mount5]}"
wan_overridden="${info[mount6]}"

# Give a file an ACL and an ordinary attribute through a mount left at the defaults.
cd "$default"
touch file
chmod 644 file
assert_success setfacl -m u:saunafstest_1:rwx file
assert_success setfattr -n user.colour -v blue file
assert_matches "user:saunafstest_1:rwx" "$(getfacl -cpE file)"
assert_equals blue "$(getfattr --only-values -n user.colour file)"

# sfswan expands to every one of the individual options rather than being parsed
# and ignored, and a mount without it keeps the defaults.
expect_equals 0 "$(mount_option sfsacl "$wan")"
expect_equals 0 "$(mount_option sfsxattrs "$wan")"
expect_equals 1 "$(mount_option sfschunkserverlatencysort "$wan")"
expect_matches '^60(\.0*)?$' "$(mount_option sfsdirentrycacheto "$wan")"
expect_equals 10000000 "$(mount_option sfsdirentrycachesize "$wan")"

expect_equals 1 "$(mount_option sfsacl "$default")"
expect_equals 1 "$(mount_option sfsxattrs "$default")"
expect_equals 0 "$(mount_option sfschunkserverlatencysort "$default")"

# Options given after the preset win, and the rest of the preset survives them,
# so sfswan is a starting point rather than a lock.
expect_equals 1 "$(mount_option sfsacl "$wan_overridden")"
expect_equals 1 "$(mount_option sfsxattrs "$wan_overridden")"
expect_equals 1 "$(mount_option sfschunkserverlatencysort "$wan_overridden")"
expect_matches '^60(\.0*)?$' "$(mount_option sfsdirentrycacheto "$wan_overridden")"
expect_equals 10000000 "$(mount_option sfsdirentrycachesize "$wan_overridden")"

# sfswan=0 mounts successfully and leaves every one of those options exactly as
# the stock mount has them, so it can sit in a config file.
for option in sfsacl sfsxattrs sfschunkserverlatencysort sfsdirentrycacheto \
		sfsdirentrycachesize; do
	expect_equals "$(mount_option "$option" "$default")" \
		"$(mount_option "$option" "$wan_disabled")"
done
expect_equals blue "$(getfattr --only-values -n user.colour "$wan_disabled/file")"

# The override reaches behaviour too: attributes and ACLs both work there.
overridden_acl="$(getfacl -cpE "$wan_overridden/file")"
expect_matches "user:saunafstest_1:rwx" "$overridden_acl"
expect_equals blue "$(getfattr --only-values -n user.colour "$wan_overridden/file")"

# With xattrs disabled every attribute request is refused and the list is empty.
for mount in "$no_xattrs" "$wan"; do
	expect_failure setfattr -n user.shape -v round "$mount/file"
	expect_matches "Operation not supported" \
		"$(setfattr -n user.shape -v round "$mount/file" 2>&1 || true)"
	expect_matches "Operation not supported" \
		"$(getfattr -n user.colour "$mount/file" 2>&1 || true)"
	expect_failure setfattr -x user.colour "$mount/file"
	# Listing reports nothing rather than failing, so a caller walking a tree keeps going.
	names="$(xattr_names "$mount/file")"
	expect_empty "$names"
	# Ordinary file operations are untouched by the option.
	contents="$(cat "$mount/file")"
	expect_empty "$contents"
	assert_success touch "$mount/file"
done

# With only ACLs disabled the ACLs become invisible but ordinary attributes still work,
# which is what separates sfsacl from sfsxattrs.
expect_matches "Operation not supported" \
	"$(getfattr -n system.posix_acl_access "$no_acls/file" 2>&1 || true)"
expect_failure setfacl -m u:saunafstest_2:rwx "$no_acls/file"
acl_text="$(getfacl -cpE "$no_acls/file")"
expect_equals 0 "$(grep -c saunafstest_1 <<< "$acl_text" || true)"

names="$(xattr_names "$no_acls/file")"
expect_equals 0 "$(grep -c posix_acl <<< "$names" || true)"
expect_matches "user.colour" "$names"
expect_equals blue "$(getfattr --only-values -n user.colour "$no_acls/file")"
assert_success setfattr -n user.shape -v round "$no_acls/file"

# None of the refusals reached the master: the ACL and the attribute are still there.
cd "$default"
expect_matches "user:saunafstest_1:rwx" "$(getfacl -cpE file)"
expect_equals blue "$(getfattr --only-values -n user.colour file)"
expect_equals round "$(getfattr --only-values -n user.shape file)"

# Latency sorting must not disturb reads and writes, including when the set of
# chunkservers to score changes underneath it.
cd "$latency_sorted"
mkdir dir
saunafs setgoal 2 dir
FILE_SIZE=$(( 2 * SAUNAFS_CHUNK_SIZE )) file-generate dir/file
assert_success file-validate dir/file

saunafs_chunkserver_daemon 0 stop
saunafs_wait_for_ready_chunkservers 2
assert_success file-validate dir/file
FILE_SIZE=$(( 2 * SAUNAFS_CHUNK_SIZE )) file-generate dir/second
assert_success file-validate dir/second

saunafs_chunkserver_daemon 0 start
saunafs_wait_for_ready_chunkservers 3
assert_success file-validate dir/file
assert_success file-validate dir/second

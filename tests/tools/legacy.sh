# This was used for testing older MooseFS versions.
# This is still used in some tests, and should be removed.
build_legacy_or_use_cache() {
	local patch_path="${SOURCE_DIR}"/tests/tools/legacy_valgrind.patch

	# Exit if legacy was already configured and installed,
	# assume it was configured properly
	(cd "$legacy_DIR/src/sfs-1.6.27" && make install) && return || true

	rm -rf "$legacy_DIR"
	mkdir -p "$legacy_DIR"
	pushd "$legacy_DIR"
	mkdir src
	cd src
	wget http://legacy.org/tl_files/sfscode/sfs-1.6.27-5.tar.gz
	tar xzf sfs-1.6.27-5.tar.gz
	cd sfs-1.6.27
	patch -p1 <$patch_path
	./configure --prefix="$legacy_DIR"
	make install
	popd
}

test_legacy() {
	test -x "$legacy_DIR/bin/sfsmount"
	test -x "$legacy_DIR/sbin/sfschunkserver"
	test -x "$legacy_DIR/sbin/sfsmaster"
}

build_legacy() {
	build_legacy_or_use_cache
	test_legacy
}

legacy_chunkserver_daemon() {
	"$legacy_DIR/sbin/sfschunkserver" -c "${saunafs_info_[chunkserver$1_cfg]}" "$2" | cat
	return ${PIPESTATUS[0]}
}

legacy_master_daemon() {
	"$legacy_DIR/sbin/sfsmaster" -c "${saunafs_info_[master_cfg]}" "$1" | cat
	return ${PIPESTATUS[0]}
}

# A generic function to run legacy commands. Usage examples:
# saunafs setgoal 3 file
# saunafs dirinfo file
# sfs sfsmetalogger stop
sfs() {
	local command="$1"
	shift
	"$legacy_DIR/"*bin"/$command" "$@" | cat
	return ${PIPESTATUS[0]}
}

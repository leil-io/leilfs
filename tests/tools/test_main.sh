# Enable alias expansion and clear inherited aliases.
unalias -a
shopt -s expand_aliases extdebug

command_prefix=
for i in sfsmaster sfschunkserver sfsmount sfsmetarestore sfsmetalogger; do
	alias $i="\${command_prefix} $i"
done

. tools/config.sh # This has to be the first one
. $(which set_saunafs_constants.sh)
. tools/stack_trace.sh
. tools/assert.sh
. tools/legacy.sh
. tools/string.sh
. tools/nullblk_zoned.sh
. tools/foundationdb.sh
. tools/saunafs.sh
. tools/saunafsXX.sh
. tools/network.sh
. tools/permissions.sh
. tools/random.sh
. tools/chunks.sh
. tools/system.sh
. tools/test.sh
. tools/timeout.sh # has to be sourced after assert.sh
. tools/valgrind.sh
. tools/time.sh
. tools/quota.sh
. tools/metadata.sh
. tools/color.sh
. tools/continuous_test.sh
. tools/logs.sh
. tools/ganesha.sh
. tools/tlscerts.sh

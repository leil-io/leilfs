timeout_set 70 seconds

CHUNKSERVERS=3 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER,enablefilelocks=1,cacheexpirationtime=0" \
	CHUNKSERVER_EXTRA_CONFIG="READ_AHEAD_KB = 1024|MAX_READ_BEHIND_KB = 2048"
	setup_local_empty_saunafs info

cd ${info[mount0]}

mkdir cthon_tests
export NFSTESTDIR=${info[mount0]}/cthon_tests

git clone https://github.com/leil-io/cthon04.git
cd cthon04
# cthon04's domount.c redeclares getenv() with empty parens the old K&R way.
# Starting with GCC 15, the default standard is gnu23, where C23 gives
# empty parens "(void)" semantics instead of the old "unspecified
# arguments" meaning, so that redeclaration now conflicts with
# <stdlib.h>'s real prototype and the build fails. Only pin an older
# standard on compilers new enough to need it, so earlier GCCs (22.04,
# 24.04) keep building with their own default exactly as before.
gcc_major="$(gcc -dumpversion | cut -d. -f1)"
if [ "${gcc_major}" -ge 15 ]; then
	make all CC="gcc -std=gnu17"
else
	make all
fi

./runtests -b -n
./runtests -l -n
./runtests -s -n


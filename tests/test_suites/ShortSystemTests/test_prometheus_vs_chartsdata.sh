assert_program_installed curl
timeout_set 10000 minutes

get_next_port_number port0
prometheus_host0="localhost:${port0}"
get_next_port_number port1
prometheus_host1="localhost:${port1}"
get_next_port_number port2
prometheus_host2="localhost:${port2}"
get_next_port_number port3
prometheus_host3="localhost:${port3}"

echo $prometheus_host0
echo $prometheus_host1
echo $prometheus_host2
echo $prometheus_host3

CHUNKSERVERS=4 \
	DISK_PER_CHUNKSERVER=3 \
	CGI_SERVER="YES" \
	MOUNTS=3 \
	USE_RAMDISK="YES" \
	MOUNT_0_EXTRA_CONFIG="sfscachemode=NEVER,sfsreportreservedperiod=1,sfsdirentrycacheto=0" \
	MOUNT_1_EXTRA_CONFIG="sfsmeta" \
	MOUNT_2_EXTRA_EXPORTS="mingoal=1,maxgoal=10,maxtrashtime=2w" \
    SFSEXPORTS_EXTRA_OPTIONS="allcanchangequota,ignoregid" \
    SFSEXPORTS_META_EXTRA_OPTIONS="nonrootmeta" \
	CHUNKSERVER_0_EXTRA_CONFIG="ENABLE_PROMETHEUS = 1|PROMETHEUS_HOST = ${prometheus_host0}" \
	CHUNKSERVER_1_EXTRA_CONFIG="ENABLE_PROMETHEUS = 1|PROMETHEUS_HOST = ${prometheus_host1}" \
	CHUNKSERVER_2_EXTRA_CONFIG="ENABLE_PROMETHEUS = 1|PROMETHEUS_HOST = ${prometheus_host2}" \
	CHUNKSERVER_3_EXTRA_CONFIG="ENABLE_PROMETHEUS = 1|PROMETHEUS_HOST = ${prometheus_host3}" \
	setup_local_empty_saunafs info

	# MASTER_CUSTOM_GOALS="1 1: _ |2 2: _ _ | 3 3: _ _ _ | 4 4: _ _ _ _ | 5 5: _ _ _ _ _ |10 ec_3_1: \$ec(3,1)" \
echo "Checking prometheus metrics"
assert_success curl --fail "${prometheus_host0}/metrics"
assert_success curl --fail "${prometheus_host1}/metrics"
assert_success curl --fail "${prometheus_host2}/metrics"
assert_success curl --fail "${prometheus_host3}/metrics"

cd "${info[mount0]}"
metadata_generate_all

file0="bar"
file1="foo"
echo "Doing reads and writes"
echo "foo" > "$file0"
echo "bar" > "$file1"
cat bar
cat foo

# Set different goals to simulate replications
echo "Setting goals"
saunafs setgoal ec22 "$file0"
saunafs setgoal 2 "$file1"

find . -type f -exec cat {} + > /dev/null

curl --fail "${prometheus_host0}/metrics"
curl --fail "${prometheus_host1}/metrics"
curl --fail "${prometheus_host2}/metrics"
curl --fail "${prometheus_host3}/metrics"

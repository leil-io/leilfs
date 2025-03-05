declare port
get_next_port_number port
prometheus_host="localhost:${port}"
USE_RAMDISK=YES \
	MASTERSERVERS=1 \
	MOUNTS=1 \
	CHUNKSERVERS=3
	MASTER_EXTRA_CONFIG="ENABLE_PROMETHEUS = 1|PROMETHEUS_HOST = ${prometheus_host}" \
	SFSEXPORTS_EXTRA_OPTIONS="allcanchangequota,ignoregid" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER,symlinkcachetimeout=0" \
	setup_local_empty_saunafs info

# Check if metrics is up
curl --fail "${prometheus_host}/metrics"

cd "${info[mount0]}"
metadata_generate_all
# Do some reading and linking
echo "foo" > bar
ln -s bar bar_ln
cat bar
readlink bar_ln

# Check if any value is zero
input_data=$(grep -E 'metadata_stats_total\{filesystem="operations",operation="[A-Z]+"\}' <<< "$(curl --fail "${prometheus_host}/metrics")")
echo "$input_data" | while read -r line; do
	echo "${line}"
	# BUG: Setting symlinkcachetimeout to 0, it still uses client cache
	if grep -q "READLINK" <<< "$line"; then
		continue
	fi
	# Extract the numeric value from each line
	value=$(echo "$line" | awk '{print $NF}')

	# Check if the value is greater than zero
	expect_less_than 0 "$value"
done

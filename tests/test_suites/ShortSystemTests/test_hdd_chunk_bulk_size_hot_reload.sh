timeout_set 1 minute

CHUNKSERVERS=1 \
	USE_RAMDISK=YES \
	CHUNKSERVER_EXTRA_CONFIG="HDD_CHUNK_BULK_SIZE = 1|\
		MAGIC_DEBUG_LOG = ${TEMP_DIR}/log|\
		LOG_FLUSH_ON=INFO" \
	setup_local_empty_saunafs info

apply_chunkserver_config() {
	local parameter=$1
	local value=$2
	for i in $(seq 0 $(( ${info[chunkserver_count]} - 1 ))); do
		sed -i -re "s/^$parameter = .*/$parameter = $value/" "${info[chunkserver${i}_cfg]}"
	done
}

# Verify the effective value logged at startup.
assert_eventually_prints 1 \
	"grep -m1 'Effective HDD_CHUNK_BULK_SIZE: 1' '${TEMP_DIR}/log' | wc -l"

# Change the value and hot-reload chunkserver config.
apply_chunkserver_config HDD_CHUNK_BULK_SIZE 1000
saunafs_chunkserver_daemon 0 reload

# Verify reload logs include change notification and the new effective value.
assert_eventually_prints 1 \
	"grep -m1 'HDD_CHUNK_BULK_SIZE changed from 1 to 1000' '${TEMP_DIR}/log' | wc -l"
assert_eventually_prints 1 \
	"grep -m1 'Effective HDD_CHUNK_BULK_SIZE: 1000' '${TEMP_DIR}/log' | wc -l"

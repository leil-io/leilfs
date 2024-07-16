if ! ldd $(whereis sfsmaster | awk '{print $2}') | grep prometheus; then
	echo "Prometheus client isn't linked with master, skipping test..."
	test_end
fi
expected_client_received_initial='metadata_observed_packets_client_total{direction="rx",protocol="tcp"}'
expected_client_sent_initial='metadata_observed_packets_client_total{direction="tx",protocol="tcp"}'
prometheus_host="localhost:45678"

USE_RAMDISK=YES \
	MASTERSERVERS=1 \
	CHUNKSERVERS=0 \
	MOUNTS=0 \
	AUTO_SHADOW_MASTER=NO \
	MASTER_EXTRA_CONFIG="ENABLE_PROMETHEUS = 1|PROMETHEUS_HOST = ${prometheus_host}" \
	setup_local_empty_saunafs info

curl --fail "${prometheus_host}/metrics"
curl --fail "${prometheus_host}/metrics"
# Why are these at 1 when master is started with no clients?
expect_equals 1 $(grep "${expected_client_received_initial}" <<< "$(curl --fail "${prometheus_host}/metrics")" | awk '{print $2}')
expect_equals 1 $(grep "${expected_client_sent_initial}" <<< "$(curl --fail "${prometheus_host}/metrics")" | awk '{print $2}')

# First 4 bytes: type ANTOAN_PING (check src/protocol/SFSCommunication.h)
# Second 4 bytes: byte length of data
# Last 4 bytes: Data (needs to be 4 bytes)
# TODO: Figure out how to close after receiving a reply (master keeps connection
# open for a while, sending ANTOAN_NOP)
echo -e "\x00\x00\x00\x03\x00\x00\x00\x04\x00\x00\x00\x01" | nc -q 0 127.0.0.1 ${saunafs_info_[matocl]}

expect_equals 2 $(grep "${expected_client_received_initial}" <<< "$(curl --fail "${prometheus_host}/metrics")" | awk '{print $2}')

assert_program_installed curl
timeout_set 10 minutes

get_next_port_number port
prometheus_host="localhost:${port}"

CHUNKSERVERS=3 \
	DISK_PER_CHUNKSERVER=3 \
	CGI_SERVER="YES" \
	MOUNTS=3 \
	USE_RAMDISK="YES" \
	MOUNT_0_EXTRA_CONFIG="sfscachemode=NEVER,sfsreportreservedperiod=1,sfsdirentrycacheto=0" \
	MOUNT_1_EXTRA_CONFIG="sfsmeta" \
	MOUNT_2_EXTRA_EXPORTS="mingoal=1,maxgoal=10,maxtrashtime=2w" \
    SFSEXPORTS_EXTRA_OPTIONS="allcanchangequota,ignoregid" \
    SFSEXPORTS_META_EXTRA_OPTIONS="nonrootmeta" \
	setup_local_empty_saunafs info

# MASTER_EXTRA_CONFIG="ENABLE_PROMETHEUS = 1|PROMETHEUS_HOST = ${prometheus_host}" \

cd "${info[mount0]}"
echo "${info[cgi_url]}"
charts_url="${info[cgi_url]//sfs.cgi/chart.cgi}"
echo "${charts_url}"
echo "${charts_url}&id=90180"
echo "Generating metadata..."
metadata_generate_all
echo "Metadata generated, sleeping to allow chartsdata to update"
sleep 300
echo "${charts_url}&id=91000"
curl --verbose "${charts_url}&id=91000"

cgi_pages="$RAMDISK_DIR/cgi_pages"
traverse_cgi "$cgi_pages"

cat "$cgi_pages/read_master_operations"

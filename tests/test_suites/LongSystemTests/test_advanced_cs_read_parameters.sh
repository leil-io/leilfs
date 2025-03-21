# Goal of test:
# Verify that combinations of different read parameters in chunkserver side successfully manage to
# read a file in different patterns.
#
assert_program_installed fio

timeout_set 20 minutes

CHUNKSERVERS=5 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	CHUNKSERVER_EXTRA_CONFIG="NR_OF_HDD_WORKERS_PER_NETWORK_WORKER = 1`
		`|BGJOBSCNT_PER_NETWORK_WORKER = 1000`
		`|MAX_BLOCKS_PER_HDD_READ_JOB = 1`
		`|MAX_PARALLEL_HDD_READ_JOBS_PER_CS_ENTRY = 1" \
	setup_local_empty_saunafs info

apply_chunkserver_config() {
	local parameter=$1
	local value=$2
	for i in $(seq 0 $(( ${info[chunkserver_count]} - 1 ))); do
		sed -i -re "s/^$parameter = .*/$parameter = $value/" "${info[chunkserver${i}_cfg]}"
	done
}

file_size=$(( 4 * SAUNAFS_CHUNK_SIZE ))
# Note this path can be changed to dump output to desired location
fio_output_dir=${TEMP_DIR}/fio_outputs
mkdir $fio_output_dir

cd "$TEMP_DIR"
assert_success saunafs_mount_unmount 0

current_iteration=0
for nrOfHddWorkersPerNetworkWorker in 1 4 16; do
	apply_chunkserver_config NR_OF_HDD_WORKERS_PER_NETWORK_WORKER $nrOfHddWorkersPerNetworkWorker
	for bgJobsPerNetworkWorker in 1000 4000 50000; do
		apply_chunkserver_config BGJOBSCNT_PER_NETWORK_WORKER $bgJobsPerNetworkWorker
		# To apply the new configuration, we need to restart the chunkserver.
		for i in $(seq 0 $(( ${info[chunkserver_count]} - 1 ))); do
			saunafs_chunkserver_daemon $i restart
		done
		saunafs_wait_for_all_ready_chunkservers

		for maxParallelHddReadJobs in 1 16 1024; do
			apply_chunkserver_config MAX_PARALLEL_HDD_READ_JOBS_PER_CS_ENTRY $maxParallelHddReadJobs
			for maxBlocksPerHddReadJob in 1 8 64; do
				apply_chunkserver_config MAX_BLOCKS_PER_HDD_READ_JOB $maxBlocksPerHddReadJob

				# To apply the new configuration, we just need to reload the chunkservers.
				for i in $(seq 0 $(( ${info[chunkserver_count]} - 1 ))); do
					cat "${info[chunkserver${i}_cfg]}"
					saunafs_chunkserver_daemon $i reload
				done

				output_dir=$fio_output_dir/$current_iteration
				mkdir $output_dir

				drop_caches
				echo ""
				echo "---------------------------------------------------------------------------------"
				echo "                          Single Sequential Read"
				echo "---------------------------------------------------------------------------------"
				assert_success fio -filename=file -direct=1 -output=$output_dir/ssr.txt \
					-group_reporting -rw=read -bs=1MB -size=$file_size -name=seqread -runtime=100

				drop_caches
				echo ""
				echo "---------------------------------------------------------------------------------"
				echo "                            Single Random Read"
				echo "---------------------------------------------------------------------------------"
				assert_success fio -filename=file -direct=1 -output=$output_dir/srr.txt \
					-group_reporting -rw=randread -bs=1MB -size=$file_size -name=randread -runtime=100

				drop_caches
				echo ""
				echo "---------------------------------------------------------------------------------"
				echo "                           Five Sequential Reads"
				echo "---------------------------------------------------------------------------------"
				assert_success fio -filename=file -direct=1 -output=$output_dir/sr5.txt \
					-group_reporting -rw=read -bs=1MB -size=$file_size -name=seqreads_5 -runtime=100 \
					-numjobs=5

				drop_caches
				echo ""
				echo "---------------------------------------------------------------------------------"
				echo "                             Five Random Reads"
				echo "---------------------------------------------------------------------------------"
				assert_success fio -filename=file -direct=1 -output=$output_dir/rr5.txt \
					-group_reporting -rw=randread -bs=1MB -size=$file_size -name=randreads_5 \
					-runtime=100 -numjobs=5

				drop_caches
				echo ""
				echo "---------------------------------------------------------------------------------"
				echo "             128 Slow Sequential Reads + Fast Sequential Read"
				echo "---------------------------------------------------------------------------------"
				assert_success fio -filename=file -direct=1 -output=$output_dir/ssr128_fsr.txt \
					-group_reporting -rw=read -name=slowseqreads_128 -bs=$(( $file_size / 4096 )) \
					-size=$(( $file_size / 512 )) -thinktime=125ms -runtime=2 -numjobs=128 \
					-name=fastseqread -bs=1MB -size=$file_size

				drop_caches
				echo ""
				echo "---------------------------------------------------------------------------------"
				echo "               128 Slow Random Reads + Fast Sequential Read"
				echo "---------------------------------------------------------------------------------"
				assert_success fio -filename=file -direct=1 -output=$output_dir/srr128_fsr.txt \
					-group_reporting -name=slowrandreads_128 -rw=randread -bs=$(( $file_size / 4096 )) \
					-size=$(( $file_size / 512 )) -thinktime=125ms -runtime=2 -numjobs=128 \
					-name=fastseqread -rw=read -bs=1MB -size=$file_size

				current_iteration=$(( $current_iteration + 1 ))
			done
		done
	done
done

timeout_set '30 seconds'

MOUNTS=1
setup_local_empty_saunafs info

cd "${info[mount0]}"
mkdir "${info[mount0]}/test_dir"

maxNumberOfFiles=1000

# Create 1K files
echo "Creation of 1K files started..."
seq 1 $maxNumberOfFiles | xargs -P "$(nproc)" -I{} touch "${info[mount0]}/test_dir/file_{}"
echo "Creation of 1K files finished!!!"

saunafs-admin list-sessions localhost "${info[matocl]}"

# Read the directory in background to trigger readdir
(
  while true; do
    ls "${info[mount0]}/test_dir" > /dev/null
    sleep 0.3
  done
) &

deletions=5

# Delete sessions while readdir is running
for i in $(seq 1 "$deletions"); do
    echo "Deleting session: iteration $i:"
    saunafs-admin delete-session localhost "${info[matocl]}" 1
    sleep 0.5
done

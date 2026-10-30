timeout_set 1 minutes
assert_program_installed nfs4_setfacl nfs4_getfacl
assert_program_installed setfacl getfacl

master_cfg="METADATA_DUMP_PERIOD_SECONDS = 0"
master_cfg+="|MAGIC_DEBUG_LOG = $TEMP_DIR/syslog|LOG_FLUSH_ON=TRACE"
touch "$TEMP_DIR/syslog"

export SAUNAFS_LOG_LEVEL=trace
CHUNKSERVERS=0 \
USE_RAMDISK="YES" \
ADMIN_PASSWORD="password" \
MASTER_EXTRA_CONFIG="$master_cfg" \
setup_local_empty_saunafs info

## Metadata size after initialize from empty metadata
METADATA_INIT_SIZE=296
METADATA_SECTION_HEADER_SIZE=16
METADATA_HEADER_SIZE=40
METADATA_SECTION_AMOUNT=8

function check_version_increased() {
    local version1=$1
    local version2=$2
    if [[ ${version1} -gt ${version2} ]]; then
        return 0
    fi
    return 1
}

function get_facl() {
    file=$1
    getfacl -cpE "$file" | tr "\n" " " | trim
}

function get_acl_hits() {
    grep -E "master.cltoma_fuse_getacl: $(stat -c %i $1)" "${TEMP_DIR}/syslog" | wc -l
}

function get_metadata_sections() {
    local metadata_path=$1
    local sections=$(sfsmetadump "${metadata_path}" | grep -i "length")
    echo "${sections}"
}

function get_empty_metadata_size() {
    local sections=$(get_metadata_sections "${info[master_data_path]}/metadata.leil")
    local total_size=$(echo "${sections}" | awk -v header1="${METADATA_HEADER_SIZE}" -v section_headers="$(("${METADATA_SECTION_HEADER_SIZE}" * "${METADATA_SECTION_AMOUNT}"))" '{sum+=$9} END {print header1+sum+section_headers}')
    ## Verify all empty section default sizes at Initialize from empty metadata
    assert_equals 33 "$(echo "${sections}" | grep -i "Node" | awk '{print $9}')"
    assert_equals 10 "$(echo "${sections}" | grep -i "Edge" | awk '{print $9}')"
    assert_equals 4 "$(echo "${sections}" | grep -i "Free" | awk '{print $9}')"
    assert_equals 9 "$(echo "${sections}" | grep -i "Xatr" | awk '{print $9}')"
    assert_equals 4 "$(echo "${sections}" | grep -i "ACLs" | awk '{print $9}')"
    assert_equals 8 "$(echo "${sections}" | grep -i "Quot" | awk '{print $9}')"
    assert_equals 32 "$(echo "${sections}" | grep -i "Flck" | awk '{print $9}')"
    assert_equals 28 "$(echo "${sections}" | grep -i "Chnk" | awk '{print $9}')"

    echo "${total_size}"
}

metadata_init_version="$(metadata_get_version "${info[master_data_path]}/metadata.leil")"

log_data="$(sed -n '/connected to Master/,$p' "${TEMP_DIR}/syslog" | grep -E info)"
metadata_path="$(echo ${log_data} | grep -E "*opened metadata file*"| awk -F 'metadata file ' '{print $2}' | awk '{print $1}')"
assert_file_exists "${metadata_path}"

## Verify All sections are loaded successfully
assert_equals ${METADATA_SECTION_AMOUNT} $(grep -E "Section loaded successfully" "${TEMP_DIR}/syslog" | wc -l)

assert_equals ${METADATA_INIT_SIZE} "$(stat -c %s "${metadata_path}")"
assert_equals 1 $(metadata_get_version "${info[master_data_path]}/metadata.leil")

read -r total_inodes dir_inodes file_inodes symlink_inodes chunk_amount <<< "$(echo "${log_data}" | grep -E "metadata file ${metadata_path} read" | sed -n 's/.*metadata file .* read (\(.*\) inodes including \(.*\) directory inodes, \(.*\) file inodes, \(.*\) symlink inodes and \(.*\) chunks).*/\1 \2 \3 \4 \5 /p')"

## Verify the total size of the metadata
assert_equals "${METADATA_INIT_SIZE}" "$(get_empty_metadata_size)"

cd "${info[mount0]}"

METADATA_INODES=10
## Create 10 directories at root
mkdir -p "${info[mount0]}"/dir{1..10}

## Create a file inside each directory and a symlink at root
for dir in {1..10}; do
    touch "${info[mount0]}"/dir${dir}/file${dir}
    ln -s "${info[mount0]}"/dir${dir}/file${dir} "${info[mount0]}"/symlink${dir}
    chmod 664 "${info[mount0]}"/dir${dir}/file${dir}
done

## Define ACLs for the files
file1_acl='user::rw- user:saunafstest:rwx group::rw- group:saunafstest:rwx mask::rwx other::r--'
file2_acl='user::rw- user:saunafstest:r-- group::rw- group:saunafstest:r-- mask::rw- other::---'
file3_acl='user::rw- user:saunafstest:-w- group::rw- group:saunafstest:-w- mask::rw- other::---'

## Create different ACL types for different files
setfacl -m u:saunafstest:rwx -m g:saunafstest:rwx -m o::r-- "${info[mount0]}/dir1/file1" 2> /dev/null
setfacl -m u:saunafstest:r-- -m g:saunafstest:r-- -m o::--- "${info[mount0]}/symlink2" 2> /dev/null
setfacl -m u:saunafstest:-w- -m g:saunafstest:-w- -m o::--- "${info[mount0]}/dir3/file3" 2> /dev/null

saunafs-admin save-metadata localhost "${saunafs_info_[matocl]}" <<< "password"

saunafs_info_output=$(saunafs-admin info localhost "${saunafs_info_[matocl]}")
total_inodes_before_restart="$(grep -E "FS objects:" <<< "$saunafs_info_output" | awk '{print $3}')"
dir_inodes_before_restart="$(grep -E "Directories:" <<< "$saunafs_info_output" | awk '{print $2}')"
file_inodes_before_restart="$(grep -E "Files:" <<< "$saunafs_info_output" | awk '{print $2}')"
symlink_inodes_before_restart="$(grep -E "Symlinks:" <<< "$saunafs_info_output" | awk '{print $2}')"

## Check version has increased
metadata_current_version="$(metadata_get_version "${info[master_data_path]}/metadata.leil")"
assert_success check_version_increased ${metadata_current_version} ${metadata_init_version}

## Restart master and check metadata size and inodes
assert_success saunafs_master_daemon restart

## Verify Nodes sections properly loaded after restart
log_data_after_restart="$(sed -n '/terminate signal received/,$p' "${TEMP_DIR}/syslog" | grep -E info)"
metadata_path_after_restart="$(echo "${log_data_after_restart}" | grep -E "opened metadata file"| awk -F 'metadata file ' '{print $2}' | awk '{print $1}')"
assert_file_exists "${metadata_path_after_restart}"
read -r total_inodes_after_restart dir_inodes_after_restart \
      file_inodes_after_restart symlink_inodes_after_restart \
      chunk_amount_after_restart <<< "$(echo "${log_data_after_restart}" | \
      grep -E "metadata file ${metadata_path_after_restart} read" | \
      sed -n 's/.*metadata file .* read (\(.*\) inodes including \(.*\) directory inodes, \(.*\) file inodes, \(.*\) symlink inodes and \(.*\) chunks).*/\1 \2 \3 \4 \5 /p')"

## Total Directory inodes are 10 plus the root directory
assert_equals $((${METADATA_INODES} + 1)) ${dir_inodes_before_restart}
assert_equals ${METADATA_INODES} ${file_inodes_before_restart}
assert_equals ${METADATA_INODES} ${symlink_inodes_before_restart}
assert_equals $((${METADATA_INODES} * 3 + 1)) ${total_inodes_before_restart}

## Verify that the inodes are the same before and after the restart
assert_equals ${dir_inodes_before_restart} ${dir_inodes_after_restart}
assert_equals ${file_inodes_before_restart} ${file_inodes_after_restart}
assert_equals ${symlink_inodes_before_restart} ${symlink_inodes_after_restart}
assert_equals ${total_inodes_before_restart} ${total_inodes_after_restart}

## Verify Edges sections properly loaded after restart
for i in {1..10}; do
    assert_file_exists "${info[mount0]}/dir${i}/file${i}"
    assert_file_exists "${info[mount0]}/symlink${i}"
done

## Verify master responses to the ACLs query
assert_equals 1 $(get_acl_hits "${info[mount0]}/dir1/file1")
assert_equals 0 $(get_acl_hits "${info[mount0]}/symlink2")
assert_equals 1 $(get_acl_hits "${info[mount0]}/dir3/file3")

## Verify ACLs sections properly loaded after restart
assert_equals "$(get_facl "${info[mount0]}/dir1/file1")" "$file1_acl"
assert_equals "$(get_facl "${info[mount0]}/dir2/file2")" "$file2_acl"
assert_equals "$(get_facl "${info[mount0]}/dir3/file3")" "$file3_acl"

## Verify NFS4_ACL sections properly loaded after restart
assert_equals 2 $(nfs4_getfacl "${info[mount0]}/dir1/file1" | grep saunafstest | wc -l)

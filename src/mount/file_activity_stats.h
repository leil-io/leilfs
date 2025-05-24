#ifndef SRC_MOUNT_FILE_ACTIVITY_STATS_H_
#define SRC_MOUNT_FILE_ACTIVITY_STATS_H_

#include <cstdint>
#include <string>
#include <map>
#include <set>
#include <vector>
#include <memory> // For std::shared_ptr
#include <mutex>  // For std::mutex

namespace MyProject { // Assuming a namespace, replace if not needed or different

struct FileActivityStat {
    uint64_t inode = 0;
    std::string full_path;
    uint64_t bytes_read = 0;
    uint64_t bytes_written = 0;
    uint32_t opens = 0;
    uint32_t reads = 0;
    uint32_t writes = 0;
    uint32_t stats_ops = 0; // Renamed from 'stats'
    uint32_t other_ops = 0;
    std::map<uint32_t, uint64_t> block_size_histogram;
    uint64_t priority = 0;

    void calculate_priority() {
        priority = (reads + writes) * 1000000 + (bytes_read + bytes_written) / 1000 + other_ops + stats_ops;
    }
};

class TopFileStatsManager {
public:
    TopFileStatsManager();
    void record_open(uint64_t ino, const std::string& full_path);
    void record_read(uint64_t ino, const std::string& full_path, size_t bytes_read_count, uint32_t block_size); // Renamed bytes_read to bytes_read_count to avoid conflict
    void record_write(uint64_t ino, const std::string& full_path, size_t bytes_written_count, uint32_t block_size); // Renamed bytes_written to bytes_written_count
    void record_stat_op(uint64_t ino, const std::string& full_path);
    void record_other_op(uint64_t ino, const std::string& full_path);
    std::vector<std::shared_ptr<FileActivityStat>> get_top_files() const;

private:
    static constexpr size_t kMaxTopFiles = 1000;

    std::map<uint64_t, FileActivityStat> stats_by_inode_;
    // Stores pairs of {priority, inode} for sorting.
    // std::set sorts by the first element, then by the second in case of ties.
    // This means if two files have the same priority, the one with the lower inode will be considered "smaller".
    std::set<std::pair<uint64_t, uint64_t>> top_files_sorted_;
    mutable std::mutex mutex_; // Marked mutable to allow locking in const get_top_files()

    void update_top_files(uint64_t ino);
};

} // namespace MyProject

#endif // SRC_MOUNT_FILE_ACTIVITY_STATS_H_

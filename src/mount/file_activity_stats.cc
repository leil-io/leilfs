#include "src/mount/file_activity_stats.h"

#include <mutex>
#include <algorithm> // For std::reverse
#include <vector>
#include <memory>
#include <set>
#include <map>

namespace MyProject {

TopFileStatsManager::TopFileStatsManager() = default; // Or any specific initialization if needed

void TopFileStatsManager::record_open(uint64_t ino, const std::string& full_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    FileActivityStat& stat = stats_by_inode_[ino];
    stat.inode = ino; // Ensure inode is set
    if (stat.full_path.empty() && !full_path.empty()) {
        stat.full_path = full_path;
    }
    stat.opens++;
    update_top_files(ino);
}

void TopFileStatsManager::record_read(uint64_t ino, const std::string& full_path, size_t bytes_read_count, uint32_t block_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    FileActivityStat& stat = stats_by_inode_[ino];
    stat.inode = ino; 
    if (stat.full_path.empty() && !full_path.empty()) {
        stat.full_path = full_path;
    }
    stat.reads++;
    stat.bytes_read += bytes_read_count;
    if (block_size > 0) { // Only record if block_size is meaningful
        stat.block_size_histogram[block_size]++;
    }
    update_top_files(ino);
}

void TopFileStatsManager::record_write(uint64_t ino, const std::string& full_path, size_t bytes_written_count, uint32_t block_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    FileActivityStat& stat = stats_by_inode_[ino];
    stat.inode = ino;
    if (stat.full_path.empty() && !full_path.empty()) {
        stat.full_path = full_path;
    }
    stat.writes++;
    stat.bytes_written += bytes_written_count;
    if (block_size > 0) { // Only record if block_size is meaningful
        stat.block_size_histogram[block_size]++;
    }
    update_top_files(ino);
}

void TopFileStatsManager::record_stat_op(uint64_t ino, const std::string& full_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    FileActivityStat& stat = stats_by_inode_[ino];
    stat.inode = ino;
    if (stat.full_path.empty() && !full_path.empty()) {
        stat.full_path = full_path;
    }
    stat.stats_ops++;
    update_top_files(ino);
}

void TopFileStatsManager::record_other_op(uint64_t ino, const std::string& full_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    FileActivityStat& stat = stats_by_inode_[ino];
    stat.inode = ino;
    if (stat.full_path.empty() && !full_path.empty()) {
        stat.full_path = full_path;
    }
    stat.other_ops++;
    update_top_files(ino);
}

void TopFileStatsManager::update_top_files(uint64_t ino) {
    // Assumes mutex_ is already locked by the calling record_* method.
    
    auto it_stat = stats_by_inode_.find(ino);
    if (it_stat == stats_by_inode_.end()) {
        // This should not happen if record_* methods correctly create the entry.
        // However, if it could, we might need to create it here or log an error.
        // For now, assume it always exists.
        return; 
    }
    FileActivityStat& stat = it_stat->second;

    // Remove old entry from top_files_sorted_
    // The old priority is currently stored in stat.priority
    // Check if the file was already in top_files_sorted_
    // An iterator to the element {stat.priority, ino}
    auto old_entry_in_top_files = top_files_sorted_.find({stat.priority, ino});
    bool was_in_top_files = (old_entry_in_top_files != top_files_sorted_.end());
    
    if (was_in_top_files) {
        top_files_sorted_.erase(old_entry_in_top_files);
    }

    // Recalculate priority
    stat.calculate_priority(); // This updates stat.priority to the new value

    // Insert new entry into top_files_sorted_
    // Only insert if its priority is high enough or if there's space.
    // If top_files_sorted_ has less than kMaxTopFiles elements, always add.
    // If it's full, only add if new priority is greater than the smallest priority in the set.
    if (top_files_sorted_.size() < kMaxTopFiles || 
        (top_files_sorted_.size() == kMaxTopFiles && stat.priority > top_files_sorted_.begin()->first) ||
        (top_files_sorted_.size() == kMaxTopFiles && stat.priority == top_files_sorted_.begin()->first && ino < top_files_sorted_.begin()->second) // Tie-breaking for same lowest priority
        ) {
        
        top_files_sorted_.insert({stat.priority, ino});

        // Enforce kMaxTopFiles limit
        if (top_files_sorted_.size() > kMaxTopFiles) {
            // Get the element with the lowest priority (and smallest inode in case of tie)
            auto evicted_entry = *top_files_sorted_.begin();
            top_files_sorted_.erase(top_files_sorted_.begin());
            
            // If the evicted file is not the one we are currently updating, remove its stats.
            // If it IS the one we are updating (ino == evicted_entry.second), its stats are kept 
            // (because it's still the 'current' file being operated on, it just didn't make the top list cut).
            // However, the problem statement implies eviction means removing from stats_by_inode_ too.
            // Let's stick to: if a file is evicted from top_files_sorted_, it's also removed from stats_by_inode_.
             stats_by_inode_.erase(evicted_entry.second);
        }
    } else {
        // The file (ino) is not in the top kMaxTopFiles and doesn't qualify to be added.
        // If it wasn't in top_files_sorted_ before (was_in_top_files == false), its stats are in stats_by_inode_ but not in top_files_sorted_.
        // If it WAS in top_files_sorted_ (was_in_top_files == true), it has now been removed by the erase earlier,
        // and since it does not qualify to be re-added, it means it dropped out of the top list.
        // In this case, we should remove its stats from stats_by_inode_ as it's no longer a "top file".
        if (was_in_top_files) { // It was in the top list, but no longer qualifies
             stats_by_inode_.erase(ino);
        }
        // If !was_in_top_files, it means it was never a top file and still isn't, so stats_by_inode_
        // might contain it if it's a new file that never made the cut.
        // To ensure stats_by_inode_ only contains top files (or files being actively considered):
        // If we reach here, and `ino` is not in `top_files_sorted_`, we should remove it from `stats_by_inode_`.
        // Let's refine: `stats_by_inode_` should reflect `top_files_sorted_` plus any files that were just updated
        // but didn't make the cut. The eviction logic above should handle removing files that drop off.
        // If a file is updated but its new priority isn't high enough, and it wasn't in the list before,
        // it means it's a "new" file that doesn't make the cut. We can erase it from stats_by_inode_.
        // If it *was* in the list, it's already erased by the `if (was_in_top_files)` block.
        // The key is that `stats_by_inode_` should not grow indefinitely with non-top files.
        
        // Check if 'ino' is still present in top_files_sorted_ after potential eviction.
        // This is a bit tricky. The goal is: stats_by_inode_ should only contain stats for files currently in top_files_sorted_.
        // The current logic:
        // 1. Remove old entry of `ino` from `top_files_sorted_` if it existed.
        // 2. Recalculate priority for `ino`.
        // 3. Try to insert `ino` into `top_files_sorted_`.
        // 4. If `top_files_sorted_` is now too large, evict the smallest. The evicted one is removed from `stats_by_inode_`.
        // What if `ino` was not inserted (because its priority was too low and list was full)?
        //    - If `ino` was previously in `top_files_sorted_` (was_in_top_files = true), it was removed at step 1. Now it's not re-inserted.
        //      It should be removed from `stats_by_inode_`. This is covered by `if(was_in_top_files)` and it's not reinserted.
        //    - If `ino` was NOT previously in `top_files_sorted_` (was_in_top_files = false), and it's not inserted now,
        //      `stats_by_inode_[ino]` still exists from the record_* method. It should be removed.

        bool is_ino_in_top_files_now = false;
        for(const auto& p : top_files_sorted_){
            if(p.second == ino) {
                is_ino_in_top_files_now = true;
                break;
            }
        }
        if (!is_ino_in_top_files_now) {
            stats_by_inode_.erase(ino);
        }
    }
}


std::vector<std::shared_ptr<FileActivityStat>> TopFileStatsManager::get_top_files() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::shared_ptr<FileActivityStat>> top_files;
    top_files.reserve(top_files_sorted_.size());

    // Iterate in ascending order (default for std::set)
    for (const auto& pair : top_files_sorted_) {
        auto it = stats_by_inode_.find(pair.second);
        if (it != stats_by_inode_.end()) {
            // Create a shared_ptr to a copy of the FileActivityStat object
            top_files.push_back(std::make_shared<FileActivityStat>(it->second));
        }
    }
    // Reverse to get descending order by priority
    std::reverse(top_files.begin(), top_files.end());
    return top_files;
}

} // namespace MyProject

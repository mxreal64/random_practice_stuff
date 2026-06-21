// Copyright (C) 2026 mxreal64
// Licensed under the GNU General Public License v3

export module QueryEngine;

import std;

#if !defined(__x86_64__) && !defined(_M_X64)
#error "The vectorized query engine demands a native x86_64 hardware execution pipeline."
#endif

// Pull in the raw x86 SIMD intrinsics header via standard system layout
#include <immintrin.h>

extern "C" {
    int open(const char* pathname, int flags) noexcept;
    void* mmap(void* addr, size_t length, int prot, int flags, int fd, long offset) noexcept;
    int munmap(void* addr, size_t length) noexcept;
    int close(int fd) noexcept;
    long lseek(int fd, long offset, int whence) noexcept;
}

constexpr int O_RDONLY   = 00000000;
constexpr int PROT_READ  = 0x1;
constexpr int MAP_SHARED = 0x01;
constexpr int SEEK_END   = 2;

export struct QueryMatch {
    uint64_t timestamp;
    uint8_t level;
    std::string_view message;
};

export class VectorizedQueryEngine {
private:
    int file_fd_{-1};
    std::byte* mapped_file_{nullptr};
    std::size_t file_size_{0};

public:
    VectorizedQueryEngine() noexcept = default;
    
    ~VectorizedQueryEngine() {
        if (mapped_file_ != nullptr) ::munmap(mapped_file_, file_size_);
        if (file_fd_ >= 0) ::close(file_fd_);
    }

    bool load_storage_file(const char* filepath) noexcept {
        file_fd_ = ::open(filepath, O_RDONLY);
        if (file_fd_ < 0) [[unlikely]] return false;

        long sz = ::lseek(file_fd_, 0, SEEK_END);
        if (sz <= 0) return false;
        file_size_ = static_cast<std::size_t>(sz);

        mapped_file_ = static_cast<std::byte*>(::mmap(nullptr, file_size_, PROT_READ, MAP_SHARED, file_fd_, 0));
        return mapped_file_ != reinterpret_cast<void*>(-1);
    }

    // SIMD Vectorized Filter Scan: Scans 32 rows simultaneously per iteration
    template <typename MatchHandler>
    std::size_t execute_level_filter(uint8_t target_level, std::size_t total_rows, MatchHandler&& handler) noexcept {
        if (mapped_file_ == nullptr || total_rows == 0) return 0;

        // Calculate internal structural offsets matching ColumnarStorage layouts
        std::size_t timestamp_column_bytes = total_rows * sizeof(uint64_t);
        
        const auto* timestamps = reinterpret_cast<const uint64_t*>(mapped_file_);
        const auto* levels     = reinterpret_cast<const uint8_t*>(mapped_file_ + timestamp_column_bytes);
        const char* messages   = reinterpret_cast<const char*>(mapped_file_ + timestamp_column_bytes + (total_rows * sizeof(uint8_t)));

        std::size_t match_count = 0;
        
        // Broadcast target log level integer across all 32 slots of a 256-bit register
        __m256i target_vec = _mm256_set1_epi8(static_cast<char>(target_level));

        std::size_t i = 0;
        // Hot loop: Unrolled to scan 32 bytes of log levels at once
        for (; i + 32 <= total_rows; i += 32) {
            // Load 32 sequential bytes of log levels from RAM into an AVX2 register
            __m256i level_vec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&levels[i]));

            // Execute parallel bitwise comparison in a single clock cycle
            __m256i cmp_mask = _mm256_cmpeq_epi8(level_vec, target_vec);

            // Compress the 256-bit wide mask down into a single 32-bit integer scalar register
            uint32_t bitmask = static_cast<uint32_t>(_mm256_movemask_epi8(cmp_mask));

            // Fast-path hint: If bitmask is zero, absolutely zero records matched in this entire 32-row batch
            if (bitmask == 0) continue;

            // Process individual bits using standard hardware instruction optimizations
            while (bitmask != 0) {
                // Count trailing zeros (tzcnt / bsf) to instantly find the next matching index bit
                int relative_idx = __builtin_ctz(bitmask);
                std::size_t global_idx = i + relative_idx;

                // Fire the callback handler, passing zero-copy data pointers
                std::forward<MatchHandler>(handler)(timestamps[global_idx], levels[global_idx]);
                
                ++match_count;
                bitmask &= (bitmask - 1); // Delete the lowest processed bit in a single cycle
            }
        }

        // Clean up remaining scalar rows if total_rows is not a perfect multiple of 32
        for (; i < total_rows; ++i) {
            if (levels[i] == target_level) {
                std::forward<MatchHandler>(handler)(timestamps[i], levels[i]);
                ++match_count;
            }
        }

        return match_count;
    }
};

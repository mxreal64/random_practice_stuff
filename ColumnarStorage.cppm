// Copyright (C) 2026 mxreal64
// Licensed under the GNU General Public License v3

export module ColumnarStorage;

import std;
import StreamAggregator;

#if !defined(__x86_64__) && !defined(_M_X64)
#error "The columnar storage engine requires a native x86_64 instruction pipeline."
#endif

extern "C" {
    int open(const char* pathname, int flags, uint32_t mode) noexcept;
    long write(int fd, const void* buf, size_t count) noexcept;
    int close(int fd) noexcept;
}

constexpr int O_WRONLY    = 00000001;
constexpr int O_CREAT     = 00000100;
constexpr int O_TRUNC     = 00001000;
constexpr int O_DIRECT    = 00040000; 

export template <std::size_t RowGroupCapacity>
class ColumnarStorageEngine {
private:
    int file_descriptor_{-1};
    
    alignas(4096) std::array<uint64_t, RowGroupCapacity> timestamp_column_{};
    alignas(4096) std::array<uint8_t, RowGroupCapacity>  level_column_{};
    
    alignas(4096) std::array<std::byte, RowGroupCapacity * 256> message_payload_pool_{};
    std::size_t payload_cursor_{0};
    std::size_t current_row_count_{0};

    std::unordered_map<std::string_view, uint8_t> dictionary_encoder_{
        {"INFO", 0}, {"WARN", 1}, {"ERROR", 2}, {"DEBUG", 3}
    };

public:
    ColumnarStorageEngine() noexcept = default;
    
    ~ColumnarStorageEngine() {
        if (file_descriptor_ >= 0) ::close(file_descriptor_);
    }

    bool create_storage_file(const char* filepath) noexcept {
        file_descriptor_ = ::open(filepath, O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0644);
        return file_descriptor_ >= 0;
    }

    void ingest_record_to_columns(const std::byte* record_payload, uint32_t length) noexcept {
        if (current_row_count_ >= RowGroupCapacity) [[unlikely]] {
            flush_column_block_to_disk();
        }

        uint32_t tsc_aux;
        uint64_t hardware_timestamp = __builtin_ia32_rdtscp(&tsc_aux);

        std::string_view raw_log(reinterpret_cast<const char*>(record_payload), length);
        std::size_t delimiter_pos = raw_log.find(' ');

        uint8_t numeric_level = 0; 
        std::string_view message_content = raw_log;

        if (delimiter_pos != std::string_view::npos) {
            std::string_view level_str = raw_log.substr(0, delimiter_pos);
            auto it = dictionary_encoder_.find(level_str);
            if (it != dictionary_encoder_.end()) {
                numeric_level = it->second;
            }
            message_content = raw_log.substr(delimiter_pos + 1);
        }

        timestamp_column_[current_row_count_] = hardware_timestamp;
        level_column_[current_row_count_]     = numeric_level;

        if (payload_cursor_ + message_content.length() <= message_payload_pool_.size()) [[likely]] {
            std::memcpy(message_payload_pool_.data() + payload_cursor_, message_content.data(), message_content.length());
            payload_cursor_ += message_content.length();
        }

        ++current_row_count_;
    }

    void flush_column_block_to_disk() noexcept {
        if (current_row_count_ == 0 || file_descriptor_ < 0) return;

        std::size_t timestamp_bytes = current_row_count_ * sizeof(uint64_t);
        ::write(file_descriptor_, timestamp_column_.data(), (timestamp_bytes + 4095) & ~4095);

        std::size_t level_bytes = current_row_count_ * sizeof(uint8_t);
        ::write(file_descriptor_, level_column_.data(), (level_bytes + 4095) & ~4095);

        ::write(file_descriptor_, message_payload_pool_.data(), (payload_cursor_ + 4096) & ~4096);

        current_row_count_ = 0;
        payload_cursor_ = 0;
    }
};

// Copyright (C) 2026 mxreal64
// Licensed under the GNU General Public License v3

export module LogWatcher;

import std;
import LogShipper;

#if !defined(__x86_64__) && !defined(_M_X64)
#error "This performance tracking engine requires a native x86_64 instruction pipeline."
#endif

extern "C" {
    int inotify_init1(int flags) noexcept;
    int inotify_add_watch(int fd, const char* pathname, uint32_t mask) noexcept;
    int open(const char* pathname, int flags) noexcept;
    long lseek(int fd, long offset, int whence) noexcept;
    void* mmap(void* addr, size_t length, int prot, int flags, int fd, long offset) noexcept;
    int munmap(void* addr, size_t length) noexcept;
    int close(int fd) noexcept;
    long read(int fd, void* buf, size_t count) noexcept;
}

constexpr int IN_MODIFY      = 0x00000002;
constexpr int IN_NONBLOCK    = 00004000;
constexpr int O_RDONLY       = 00000000;
constexpr int PROT_READ      = 0x1;
constexpr int MAP_SHARED     = 0x01;
constexpr int SEEK_END       = 2;

struct inotify_event {
    int wd;
    uint32_t mask;
    uint32_t cookie;
    uint32_t len;
    char name[];
};

export class ZeroAllocLogWatcher {
private:
    int inotify_fd_{-1};
    int watch_descriptor_{-1};
    int log_file_fd_{-1};
    
    std::byte* mmap_ptr_{nullptr};
    std::size_t last_known_size_{0};
    std::size_t current_mmap_capacity_{0};

    BareMetalShipper& shipper_;

    void sync_memory_map() noexcept {
        long target_size = ::lseek(log_file_fd_, 0, SEEK_END);
        if (target_size <= static_cast<long>(last_known_size_)) return;

        if (static_cast<std::size_t>(target_size) > current_mmap_capacity_) {
            if (mmap_ptr_ != nullptr) {
                ::munmap(mmap_ptr_, current_mmap_capacity_);
            }
            current_mmap_capacity_ = ((target_size / (1024 * 1024)) + 1) * (1024 * 1024);
            mmap_ptr_ = static_cast<std::byte*>(::mmap(nullptr, current_mmap_capacity_, PROT_READ, MAP_SHARED, log_file_fd_, 0));
        }

        if (mmap_ptr_ == reinterpret_cast<void*>(-1)) [[unlikely]] {
            mmap_ptr_ = nullptr;
            return;
        }

        std::size_t new_bytes_count = target_size - last_known_size_;
        std::string_view fresh_log_window(reinterpret_cast<const char*>(mmap_ptr_ + last_known_size_), new_bytes_count);

        std::size_t line_start = 0;
        while (true) {
            std::size_t newline_pos = fresh_log_window.find('\n', line_start);
            if (newline_pos == std::string_view::npos) {
                break;
            }

            std::string_view single_log_line = fresh_log_window.substr(line_start, newline_pos - line_start);
            if (!single_log_line.empty()) {
                shipper_.ship_log(single_log_line.data(), static_cast<uint32_t>(single_log_line.length()));
            }
            line_start = newline_pos + 1;
        }

        last_known_size_ += line_start;
    }

public:
    explicit ZeroAllocLogWatcher(BareMetalShipper& shipper) noexcept 
        : shipper_(shipper) 
    {
        inotify_fd_ = ::inotify_init1(IN_NONBLOCK);
    }

    ~ZeroAllocLogWatcher() {
        if (mmap_ptr_ != nullptr) ::munmap(mmap_ptr_, current_mmap_capacity_);
        if (log_file_fd_ >= 0) ::close(log_file_fd_);
        if (inotify_fd_ >= 0) ::close(inotify_fd_);
    }

    bool initialize_target_file(const char* filepath) noexcept {
        log_file_fd_ = ::open(filepath, O_RDONLY);
        if (log_file_fd_ < 0) [[unlikely]] return false;

        long initial_size = ::lseek(log_file_fd_, 0, SEEK_END);
        last_known_size = static_cast<std::size_t>(initial_size);

        watch_descriptor_ = ::inotify_add_watch(inotify_fd_, filepath, IN_MODIFY);
        return watch_descriptor_ >= 0;
    }

    void poll_events_pump() noexcept {
        alignas(alignof(inotify_event)) std::array<std::byte, 4096> event_buffer;
        
        while (true) {
            long read_bytes = ::read(inotify_fd_, event_buffer.data(), event_buffer.size());
            if (read_bytes <= 0) break;

            std::size_t progression = 0;
            while (progression < static_cast<std::size_t>(read_bytes)) {
                auto* event = reinterpret_cast<inotify_event*>(&event_buffer[progression]);
                
                if (event->mask & IN_MODIFY) {
                    sync_memory_map();
                }
                progression += sizeof(inotify_event) + event->len;
            }
        }
    }
};

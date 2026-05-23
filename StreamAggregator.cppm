export module StreamAggregator;

import <cstdint>;
import <cstddef>;
import <atomic>;
import <utility>;
import <cstring>;
import <algorithm>;

export struct alignas(16) LogRecord {
    uint32_t length;
    uint32_t flags;
    std::atomic<uint32_t> status;
};

export template <std::size_t BufferCapacity>
class HighThroughputLogAggregator {
private:
    static_assert((BufferCapacity & (BufferCapacity - 1)) == 0, "Capacity must be a power of two.");
    static constexpr std::size_t Mask = BufferCapacity - 1;
    
    static constexpr uint32_t FlagWrap = 0x1;
    
    static constexpr uint32_t StatusWriting = 0x1;
    static constexpr uint32_t StatusReady = 0x2;

    alignas(64) std::byte storage_[BufferCapacity];
    
    alignas(64) std::atomic<uint64_t> write_head_{0};
    alignas(64) std::atomic<uint64_t> commit_head_{0};
    alignas(64) std::atomic<uint64_t> read_head_{0};

public:
    HighThroughputLogAggregator() noexcept = default;
    ~HighThroughputLogAggregator() = default;

    HighThroughputLogAggregator(const HighThroughputLogAggregator&) = delete;
    HighThroughputLogAggregator& operator=(const HighThroughputLogAggregator&) = delete;
    HighThroughputLogAggregator(HighThroughputLogAggregator&&) = delete;
    HighThroughputLogAggregator& operator=(HighThroughputLogAggregator&&) = delete;

    bool append(const void* data, uint32_t length) noexcept {
        const uint32_t total_needed = sizeof(LogRecord) + length;
        if (total_needed > BufferCapacity) {
            return false;
        }

        uint64_t current_write = write_head_.load(std::memory_order_relaxed);

        while (true) {
            uint64_t current_read = read_head_.load(std::memory_order_acquire);
            if ((current_write - current_read) + total_needed > BufferCapacity) {
                return false;
            }

            uint64_t write_idx = current_write & Mask;
            uint64_t next_write = current_write + total_needed;
            bool wrap_needed = (write_idx + total_needed > BufferCapacity);
            uint32_t space_to_end = 0;

            if (wrap_needed) {
                space_to_end = static_cast<uint32_t>(BufferCapacity - write_idx);
                if ((current_write - current_read) + space_to_end + total_needed > BufferCapacity) {
                    return false;
                }
                next_write = current_write + space_to_end + total_needed;
            }

            if (write_head_.compare_exchange_weak(current_write, next_write, std::memory_order_relaxed, std::memory_order_relaxed)) {
                uint64_t start_ticket = current_write;

                if (wrap_needed) {
                    auto* wrap_rec_ptr = ::new (static_cast<void*>(&storage_[write_idx])) LogRecord();
                    wrap_rec_ptr->length = 0;
                    wrap_rec_ptr->flags = FlagWrap;
                    wrap_rec_ptr->status.store(StatusWriting, std::memory_order_relaxed);
                    wrap_rec_ptr->status.store(StatusReady, std::memory_order_release);
                    
                    current_write += space_to_end;
                    write_idx = 0;
                }

                auto* data_rec_ptr = ::new (static_cast<void*>(&storage_[write_idx])) LogRecord();
                data_rec_ptr->length = length;
                data_rec_ptr->flags = 0;
                data_rec_ptr->status.store(StatusWriting, std::memory_order_relaxed);

                std::memcpy(&storage_[write_idx + sizeof(LogRecord)], data, length);
                data_rec_ptr->status.store(StatusReady, std::memory_order_release);

                uint64_t expected_commit = start_ticket;
                while (!commit_head_.compare_exchange_weak(expected_commit, current_write + total_needed, std::memory_order_release, std::memory_order_relaxed)) {
                    expected_commit = start_ticket;
                    #if defined(__x86_64__) || defined(_M_X64)
                    __builtin_ia32_pause();
                    #elif defined(__aarch64__)
                    asm volatile("yield" ::: "memory");
                    #endif
                }
                return true;
            }
        }
    }

    template <typename FlushHandler>
    std::size_t consume_batch(FlushHandler&& handler) noexcept {
        uint64_t current_read = read_head_.load(std::memory_order_relaxed);
        uint64_t current_commit = commit_head_.load(std::memory_order_acquire);

        if (current_read == current_commit) {
            return 0;
        }

        std::size_t processed_bytes = 0;

        while (current_read < current_commit) {
            uint64_t read_idx = current_read & Mask;
            auto* rec_ptr = reinterpret_cast<LogRecord*>(&storage_[read_idx]);

            while (rec_ptr->status.load(std::memory_order_acquire) != StatusReady) {
                #if defined(__x86_64__) || defined(_M_X64)
                __builtin_ia32_pause();
                #elif defined(__aarch64__)
                asm volatile("yield" ::: "memory");
                #endif
            }

            if (rec_ptr->flags & FlagWrap) {
                uint32_t skip = static_cast<uint32_t>(BufferCapacity - read_idx);
                rec_ptr->status.store(0, std::memory_order_release);
                current_read += skip;
                processed_bytes += skip;
                continue;
            }

            std::forward<FlushHandler>(handler)(&storage_[read_idx + sizeof(LogRecord)], rec_ptr->length);
            
            uint32_t step = sizeof(LogRecord) + rec_ptr->length;
            rec_ptr->status.store(0, std::memory_order_release);
            current_read += step;
            processed_bytes += step;
        }

        read_head_.store(current_read, std::memory_order_release);
        return processed_bytes;
    }
};

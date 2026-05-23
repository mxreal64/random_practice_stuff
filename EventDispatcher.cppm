export module EventDispatcher;

import <cstdint>;
import <cstddef>;
import <atomic>;
import <new>;
import <utility>;

export template <typename EventType>
struct alignas(64) EventSlot {
    EventType event_data;
    alignas(64) std::atomic<uint64_t> sequence{0};
};

export template <typename EventType, std::size_t Capacity>
class NanosecondDispatcher {
private:
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two.");
    static constexpr std::size_t Mask = Capacity - 1;

    alignas(64) EventSlot<EventType> ring_buffer_[Capacity];
    
    alignas(64) std::atomic<uint64_t> producer_sequence_{0};
    
    alignas(64) std::atomic<uint64_t> consumer_sequence_{0};

public:
    NanosecondDispatcher() {
        for (std::size_t i = 0; i < Capacity; ++i) {
            ring_buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    ~NanosecondDispatcher() = default;

    NanosecondDispatcher(const NanosecondDispatcher&) = delete;
    NanosecondDispatcher& operator=(const NanosecondDispatcher&) = delete;
    NanosecondDispatcher(NanosecondDispatcher&&) = delete;
    NanosecondDispatcher& operator=(NanosecondDispatcher&&) = delete;

    template <typename... Args>
    void publish(Args&&... args) noexcept {
        uint64_t ticket = producer_sequence_.load(std::memory_order_relaxed);
        EventSlot<EventType>* slot = nullptr;

        while (true) {
            slot = &ring_buffer_[ticket & Mask];
            uint64_t seq = slot->sequence.load(std::memory_order_acquire);
            
            if (seq == ticket) {
                if (producer_sequence_.compare_exchange_weak(ticket, ticket + 1, std::memory_order_acquire, std::memory_order_relaxed)) {
                    break;
                }
            } else {
                #if defined(__x86_64__) || defined(_M_X64)
                __builtin_ia32_pause();
                #elif defined(__aarch64__)
                asm volatile("yield" ::: "memory");
                #endif
                ticket = producer_sequence_.load(std::memory_order_relaxed);
            }
        }

        slot->event_data = EventType{std::forward<Args>(args)...};
        slot->sequence.store(ticket + 1, std::memory_order_release);
    }

    template <typename EventHandler>
    void consume_next(EventHandler&& handler) noexcept {
        uint64_t ticket = consumer_sequence_.load(std::memory_order_relaxed);
        EventSlot<EventType>* slot = nullptr;

        while (true) {
            slot = &ring_buffer_[ticket & Mask];
            uint64_t seq = slot->sequence.load(std::memory_order_acquire);

            if (seq == (ticket + 1)) {
                if (consumer_sequence_.compare_exchange_weak(ticket, ticket + 1, std::memory_order_acquire, std::memory_order_relaxed)) {
                    break;
                }
            } else {
                #if defined(__x86_64__) || defined(_M_X64)
                __builtin_ia32_pause();
                #elif defined(__aarch64__)
                asm volatile("yield" ::: "memory");
                #endif
                ticket = consumer_sequence_.load(std::memory_order_relaxed);
            }
        }

        std::forward<EventHandler>(handler)(slot->event_data);
        slot->sequence.store(ticket + Capacity, std::memory_order_release);
    }
};

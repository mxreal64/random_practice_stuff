// Copyright (C) 2026 mxreal64
// Licensed under the GNU General Public License v3

export module NetworkUnpacker;

import std;
import StreamAggregator;

#if !defined(__x86_64__) && !defined(_M_X64)
#error "The network unpacker requires a native x86_64 instruction pipeline."
#endif

#pragma pack(push, 1)
struct ProtocolHeader {
    uint32_t magic;
    uint32_t length;
    uint32_t flags;
};
#pragma pack(pop)

constexpr uint32_t ExpectedMagic = 0x4D58524C;

export template <std::size_t RingCapacity>
class ConnectionContext {
private:
    int socket_fd_{-1};
    
    alignas(64) std::array<std::byte, RingCapacity> network_ring_;
    std::size_t head_{0};
    std::size_t tail_{0};

    void consolidate_buffer() noexcept {
        if (head_ == tail_) {
            head_ = 0;
            tail_ = 0;
            return;
        }
        if (head_ > 0) {
            std::memmove(network_ring_.data(), network_ring_.data() + head_, tail_ - head_);
            tail_ -= head_;
            head_ = 0;
        }
    }

public:
    explicit ConnectionContext(int fd) noexcept : socket_fd_(fd) {}
    ~ConnectionContext() = default;

    int get_fd() const noexcept { return socket_fd_; }
    std::byte* get_write_ptr() noexcept { return network_ring_.data() + tail_; }
    std::size_t get_available_space() const noexcept { return RingCapacity - tail_; }
    
    void advance_tail(std::size_t bytes_written) noexcept { tail_ += bytes_written; }

    template <std::size_t AggregatorCapacity>
    void process_stream_buffer(HighThroughputLogAggregator<AggregatorCapacity>& aggregator) noexcept {
        while (true) {
            std::size_t readable_bytes = tail_ - head_;
            if (readable_bytes < sizeof(ProtocolHeader)) {
                break;
            }

            auto* header = reinterpret_cast<ProtocolHeader*>(network_ring_.data() + head_);
            
            if (header->magic != ExpectedMagic) [[unlikely]] {
                head_ = 0;
                tail_ = 0;
                return;
            }

            std::size_t complete_packet_size = sizeof(ProtocolHeader) + header->length;
            if (readable_bytes < complete_packet_size) {
                break;
            }

            const std::byte* payload_ptr = network_ring_.data() + head_ + sizeof(ProtocolHeader);

            bool success = aggregator.append(payload_ptr, header->length);
            if (!success) [[unlikely]] {
                break;
            }

            head_ += complete_packet_size;
        }

        consolidate_buffer();
    }
};

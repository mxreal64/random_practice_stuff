// Copyright (C) 2026 mxreal64
// Licensed under the GNU General Public License v3

export module LogShipper;

import std;

#if !defined(__x86_64__) && !defined(_M_X64)
#error "This shipping engine requires an x86_64 execution pipeline."
#endif

extern "C" {
    int socket(int domain, int type, int protocol) noexcept;
    int connect(int sockfd, const void* addr, uint32_t addrlen) noexcept;
    long send(int sockfd, const void* buf, size_t len, int flags) noexcept;
    int close(int fd) noexcept;
}

constexpr int AF_INET = 2;
constexpr int SOCK_STREAM = 1;
constexpr int MSG_NOSIGNAL = 0x4000; 

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t magic{0x4D58524C}; 
    uint32_t length{0};
    uint32_t flags{0};
};
#pragma pack(pop)

export class BareMetalShipper {
private:
    int socket_fd_{-1};
    
        struct {
        int16_t sin_family{AF_INET};
        uint16_t sin_port{0};
        uint32_t sin_addr{0};
        char sin_zero[8]{0};
    } server_address_;

public:
    BareMetalShipper() noexcept = default;
    
    ~BareMetalShipper() {
        if (socket_fd_ >= 0) ::close(socket_fd_);
    }

    bool connect_to_aggregator(uint32_t ip_be32, uint16_t port_be16) noexcept {
        socket_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (socket_fd_ < 0) [[unlikely]] return false;

        server_address_.sin_port = port_be16;
        server_address_.sin_addr = ip_be32;

        return ::connect(socket_fd_, &server_address_, sizeof(server_address_)) == 0;
    }

    bool ship_log(const void* log_data, uint32_t length, uint32_t flags = 0) noexcept {
        if (socket_fd_ < 0 || log_data == nullptr || length == 0) [[unlikely]] return false;

        PacketHeader header;
        header.length = length;
        header.flags = flags;

        long header_sent = ::send(socket_fd_, &header, sizeof(PacketHeader), MSG_NOSIGNAL);
        if (header_sent != sizeof(PacketHeader)) [[unlikely]] {
            return false; 
        }

        long body_sent = ::send(socket_fd_, log_data, length, MSG_NOSIGNAL);
        return body_sent == static_cast<long>(length);
    }
};

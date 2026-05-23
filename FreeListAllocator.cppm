// Copyright (C) 2026 mxreal64
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://gnu.org>.


export module FreeListAllocator;

import <cstdint>;
import <cstddef>;
import <atomic>;
import <new>;
import <utility>;
import <cstring>;

export template <std::size_t BlockCount>
class FixedSizeFreeList {
private:
    static_assert(BlockCount > 0, "Block count must be greater than zero.");
    static_assert(BlockCount <= 0xFFFFFFFF, "Block count exceeds 32-bit index space.");

    struct Node {
        uint32_t next_index;
        uint32_t padding;
    };

    struct TaggedIndex {
        uint32_t index;
        uint32_t tag;
    };

    alignas(64) std::byte storage_[BlockCount * 64];
    alignas(64) std::atomic<uint64_t> head_{0};

    static constexpr uint64_t pack(TaggedIndex ti) noexcept {
        return (static_cast<uint64_t>(ti.tag) << 32) | ti.index;
    }

    static constexpr TaggedIndex unpack(uint64_t val) noexcept {
        return TaggedIndex{
            .index = static_cast<uint32_t>(val & 0xFFFFFFFFULL),
            .tag = static_cast<uint32_t>(val >> 32)
        };
    }

public:
    FixedSizeFreeList() noexcept {
        for (std::size_t i = 0; i < BlockCount - 1; ++i) {
            Node n{.next_index = static_cast<uint32_t>(i + 1), .padding = 0};
            std::memcpy(&storage_[i * 64], &n, sizeof(Node));
        }
        Node last{.next_index = 0xFFFFFFFF, .padding = 0};
        std::memcpy(&storage_[(BlockCount - 1) * 64], &last, sizeof(Node));

        head_.store(pack({.index = 0, .tag = 0}), std::memory_order_relaxed);
    }

    ~FixedSizeFreeList() = default;

    FixedSizeFreeList(const FixedSizeFreeList&) = delete;
    FixedSizeFreeList& operator=(const FixedSizeFreeList&) = delete;
    FixedSizeFreeList(FixedSizeFreeList&&) = delete;
    FixedSizeFreeList& operator=(FixedSizeFreeList&&) = delete;

    void* allocate() noexcept {
        uint64_t current_head = head_.load(std::memory_order_acquire);
        while (true) {
            TaggedIndex unpacked = unpack(current_head);
            if (unpacked.index == 0xFFFFFFFF) {
                return nullptr;
            }

            Node current_node;
            std::memcpy(&current_node, &storage_[unpacked.index * 64], sizeof(Node));

            uint64_t next_head = pack({
                .index = current_node.next_index,
                .tag = unpacked.tag + 1
            });

            if (head_.compare_exchange_weak(current_head, next_head, std::memory_order_acquire, std::memory_order_acquire)) {
                return static_cast<void*>(&storage_[unpacked.index * 64]);
            }
        }
    }

    void deallocate(void* ptr) noexcept {
        if (!ptr) return;

        std::size_t offset = static_cast<std::byte*>(ptr) - storage_;
        uint32_t block_index = static_cast<uint32_t>(offset / 64);

        uint64_t current_head = head_.load(std::memory_order_relaxed);
        uint32_t last_known_next = 0xFFFFFFFF;

        while (true) {
            TaggedIndex unpacked = unpack(current_head);

            if (unpacked.index != last_known_next) {
                Node new_node{.next_index = unpacked.index, .padding = 0};
                std::memcpy(&storage_[block_index * 64], &new_node, sizeof(Node));
                last_known_next = unpacked.index;
            }

            uint64_t next_head = pack({
                .index = block_index,
                .tag = unpacked.tag + 1
            });

            if (head_.compare_exchange_weak(current_head, next_head, std::memory_order_release, std::memory_order_relaxed)) {
                break;
            }
        }
    }
};

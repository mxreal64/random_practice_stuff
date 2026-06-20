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


export module SparsePageTable;

import <cstddef>;
import <atomic>;
import <new>;
import <utility>;
import <cstring>;

export template <std::size_t BlockCount, std::size_t BlockSize = 64>
class FixedSizeFreeList {
private:
    static_assert(BlockCount > 0, "Block count must be greater than zero.");
    static_assert(BlockCount <= 0xFFFFFFFF, "Block count exceeds 32-bit index space.");
    static_assert(BlockSize >= sizeof(uint32_t) * 2, "Block size must accommodate metadata.");

    struct Node {
        uint32_t next_index;
        uint32_t padding;
    };

    struct TaggedIndex {
        uint32_t index;
        uint32_t tag;
    };

    alignas(64) std::byte storage_[BlockCount * BlockSize];
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
            std::memcpy(&storage_[i * BlockSize], &n, sizeof(Node));
        }
        Node last{.next_index = 0xFFFFFFFF, .padding = 0};
        std::memcpy(&storage_[(BlockCount - 1) * BlockSize], &last, sizeof(Node));

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
            std::memcpy(&current_node, &storage_[unpacked.index * BlockSize], sizeof(Node));

            uint64_t next_head = pack({
                .index = current_node.next_index,
                .tag = unpacked.tag + 1
            });

            if (head_.compare_exchange_weak(current_head, next_head, std::memory_order_acquire, std::memory_order_acquire)) {
                return static_cast<void*>(&storage_[unpacked.index * BlockSize]);
            }
        }
    }

    void deallocate(void* ptr) noexcept {
        if (!ptr) return;

        std::size_t offset = static_cast<std::byte*>(ptr) - storage_;
        uint32_t block_index = static_cast<uint32_t>(offset / BlockSize);

        uint64_t current_head = head_.load(std::memory_order_relaxed);
        uint32_t last_known_next = 0xFFFFFFFF;

        while (true) {
            TaggedIndex unpacked = unpack(current_head);

            if (unpacked.index != last_known_next) {
                Node new_node{.next_index = unpacked.index, .padding = 0};
                std::memcpy(&storage_[block_index * BlockSize], &new_node, sizeof(Node));
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

export template <std::size_t AllocatorBlockCount>
class LockFreeSparsePageTable {
private:
    static constexpr std::size_t L2_BITS = 4;
    static constexpr std::size_t L1_BITS = 28 - L2_BITS;
    static constexpr std::size_t L1_SIZE = 1ULL << L1_BITS;
    static constexpr std::size_t L2_SIZE = 1ULL << L2_BITS;
    static constexpr std::size_t L2_BLOCK_SIZE = L2_SIZE * sizeof(std::atomic<uint32_t>);
    
    static constexpr uint32_t UnmappedMarker = 0xFFFFFFFF;

    struct alignas(64) PageTableLevel2 {
        std::atomic<uint32_t> physical_indices[L2_SIZE];

        PageTableLevel2() noexcept {
            for (std::size_t i = 0; i < L2_SIZE; ++i) {
                physical_indices[i].store(UnmappedMarker, std::memory_order_relaxed);
            }
        }
    };

    static_assert(sizeof(PageTableLevel2) <= 64, "PageTableLevel2 exceeds block budget.");

    std::vector<std::atomic<PageTableLevel2*>> directory_;
    FixedSizeFreeList<AllocatorBlockCount, 64>& free_list_;

public:
    explicit LockFreeSparsePageTable(FixedSizeFreeList<AllocatorBlockCount, 64>& allocator) 
        : directory_(L1_SIZE), free_list_(allocator) {
        for (std::size_t i = 0; i < L1_SIZE; ++i) {
            directory_[i].store(nullptr, std::memory_order_relaxed);
        }
    }

    ~LockFreeSparsePageTable() {
        for (std::size_t i = 0; i < L1_SIZE; ++i) {
            PageTableLevel2* l2_ptr = directory_[i].load(std::memory_order_relaxed);
            if (l2_ptr) {
                l2_ptr->~PageTableLevel2();
                free_list_.deallocate(static_cast<void*>(l2_ptr));
            }
        }
    }

    LockFreeSparsePageTable(const LockFreeSparsePageTable&) = delete;
    LockFreeSparsePageTable& operator=(const LockFreeSparsePageTable&) = delete;
    LockFreeSparsePageTable(LockFreeSparsePageTable&&) = delete;
    LockFreeSparsePageTable& operator=(LockFreeSparsePageTable&&) = delete;

    bool map(uint32_t virtual_page_id, uint32_t physical_index) noexcept {
        const std::size_t l1_index = (virtual_page_id >> L2_BITS) & (L1_SIZE - 1);
        const std::size_t l2_index = virtual_page_id & (L2_SIZE - 1);

        PageTableLevel2* l2_ptr = directory_[l1_index].load(std::memory_order_acquire);
        
        if (!l2_ptr) {
            void* allocated_mem = free_list_.allocate();
            if (!allocated_mem) {
                return false;
            }
            
            PageTableLevel2* new_l2 = ::new (allocated_mem) PageTableLevel2();
            PageTableLevel2* expected = nullptr;
            
            if (!directory_[l1_index].compare_exchange_strong(expected, new_l2, std::memory_order_acq_rel, std::memory_order_acquire)) {
                new_l2->~PageTableLevel2();
                free_list_.deallocate(static_cast<void*>(new_l2));
                l2_ptr = expected;
            } else {
                l2_ptr = new_l2;
            }
        }

        uint32_t expected_mapping = UnmappedMarker;
        return l2_ptr->physical_indices[l2_index].compare_exchange_strong(
            expected_mapping, physical_index, std::memory_order_release, std::memory_order_relaxed
        );
    }

    uint32_t lookup(uint32_t virtual_page_id) const noexcept {
        const std::size_t l1_index = (virtual_page_id >> L2_BITS) & (L1_SIZE - 1);
        const std::size_t l2_index = virtual_page_id & (L2_SIZE - 1);

        PageTableLevel2* l2_ptr = directory_[l1_index].load(std::memory_order_acquire);
        if (!l2_ptr) {
            return UnmappedMarker;
        }

        return l2_ptr->physical_indices[l2_index].load(std::memory_order_acquire);
    }
};

// =============================================================================
//  file : include/librazobill/MemoryArena.hpp
//  role : monotonic bump-pointer arena allocator with 64-byte cache alignment
//  inv  : O(1) bump allocation; reset() rewinds without freeing backing blocks
// =============================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#include "librazobill/Lifetime.hpp"

namespace razobill::core {

// -- memory arena allocator ---------------------------------------------------

/// monotonic bump allocator providing zero-allocation reuse on the hot path
/// inv: bulk reset rewinds bump offsets without freeing backing heap pages
class RZ_GSL_OWNER MemoryArena {
public:
    /// contiguous memory block allocated with 64-byte cache-line alignment
    struct Block {
        void* ptr = nullptr;
        std::size_t capacity = 0;
    };

    /// construct arena with specified default block size
    /// in: defaultBlockSize capacity in bytes for new blocks (default 256 KB)
    explicit MemoryArena(std::size_t defaultBlockSize = 256 * 1024)
        : defaultBlockSize_(defaultBlockSize ? defaultBlockSize : 256 * 1024) {}

    /// destructor frees all allocated OS blocks
    ~MemoryArena() { clear(); }

    MemoryArena(const MemoryArena&) = delete;
    MemoryArena& operator=(const MemoryArena&) = delete;

    /// move constructor transfers block ownership and rewinds source
    MemoryArena(MemoryArena&& other) noexcept
        : blocks_(std::move(other.blocks_)), currentBlockIdx_(other.currentBlockIdx_), offset_(other.offset_),
          defaultBlockSize_(other.defaultBlockSize_) {
        other.currentBlockIdx_ = 0;
        other.offset_ = 0;
        other.defaultBlockSize_ = 0;
    }

    /// move assignment transfers block ownership after clearing current arena
    MemoryArena& operator=(MemoryArena&& other) noexcept {
        if (this != &other) {
            clear();
            blocks_ = std::move(other.blocks_);
            currentBlockIdx_ = other.currentBlockIdx_;
            offset_ = other.offset_;
            defaultBlockSize_ = other.defaultBlockSize_;
            other.currentBlockIdx_ = 0;
            other.offset_ = 0;
            other.defaultBlockSize_ = 0;
        }
        return *this;
    }

    /// bump allocate memory aligned to alignment boundary
    /// inv: O(1) pointer advance on fast path; amortized zero allocation
    ///      on reset
    /// in: bytes allocation size, alignment power of two (default 8 bytes)
    /// out: non-null aligned pointer; nullptr if bytes == 0
    /// own: memory owned by arena; lifetime tied to arena until reset()
    ///      or clear()
    /// thread: not thread-safe; single-threaded or thread-confined per worker
    /// err: throws std::bad_alloc on OS allocation failure or size overflow
    [[nodiscard]] void* allocate(std::size_t bytes, std::size_t alignment = 8) {
        if (bytes == 0) [[unlikely]]
            return nullptr;
        if (alignment == 0 || (alignment & (alignment - 1)) != 0) [[unlikely]] {
            alignment = 8;
        }

        // Fast path: fits into active block
        if (currentBlockIdx_ < blocks_.size()) [[likely]] {
            const auto& cur = blocks_[currentBlockIdx_];
            const std::size_t currentAddr = reinterpret_cast<std::size_t>(cur.ptr) + offset_;
            const std::size_t alignedAddr = (currentAddr + alignment - 1) & ~(alignment - 1);
            const std::size_t padding = alignedAddr - currentAddr;

            if (bytes <= cur.capacity && offset_ + padding <= cur.capacity - bytes) [[likely]] {
                offset_ += padding + bytes;
                return reinterpret_cast<void*>(alignedAddr);
            }
        }

        // Slow path: scan existing subsequent blocks or allocate new block
        while (++currentBlockIdx_ < blocks_.size()) {
            offset_ = 0;
            const auto& cur = blocks_[currentBlockIdx_];
            const std::size_t currentAddr = reinterpret_cast<std::size_t>(cur.ptr);
            const std::size_t alignedAddr = (currentAddr + alignment - 1) & ~(alignment - 1);
            const std::size_t padding = alignedAddr - currentAddr;

            if (padding <= cur.capacity && bytes <= cur.capacity - padding) {
                offset_ = padding + bytes;
                return reinterpret_cast<void*>(alignedAddr);
            }
        }

        // Prevent integer overflow when computing block capacity
        if (bytes > std::numeric_limits<std::size_t>::max() - alignment - 64) [[unlikely]] {
            throw std::bad_alloc();
        }

        // Allocate a new OS block with 64-byte cache line alignment
        const std::size_t required = (bytes + alignment > defaultBlockSize_) ? (bytes + alignment) : defaultBlockSize_;
        void* newBlock = nullptr;
#if defined(_MSC_VER) || defined(__MINGW32__)
        newBlock = _aligned_malloc(required, 64);
#else
        if (posix_memalign(&newBlock, 64, required) != 0) newBlock = nullptr;
#endif
        if (!newBlock) [[unlikely]]
            throw std::bad_alloc();

        struct BlockGuard {
            void* ptr;
            ~BlockGuard() {
                if (ptr) {
#if defined(_MSC_VER) || defined(__MINGW32__)
                    _aligned_free(ptr);
#else
                    free(ptr);
#endif
                }
            }
        } guard{newBlock};

        blocks_.push_back({newBlock, required});
        guard.ptr = nullptr; // Disarm on successful vector push_back
        currentBlockIdx_ = blocks_.size() - 1;

        const std::size_t baseAddr = reinterpret_cast<std::size_t>(newBlock);
        const std::size_t alignedBase = (baseAddr + alignment - 1) & ~(alignment - 1);
        offset_ = (alignedBase - baseAddr) + bytes;
        return reinterpret_cast<void*>(alignedBase);
    }

    /// construct trivially destructible object in arena memory
    /// inv: static assert guarantees T requires no destructor execution
    /// in: args forward arguments for T constructor
    /// out: non-null pointer to constructed object of type T
    /// own: memory managed by arena; destructor is NOT called on reset()
    /// thread: not thread-safe
    /// err: throws std::bad_alloc on allocation failure
    template <typename T, typename... Args> [[nodiscard]] T* create(Args&&... args) {
        static_assert(
            std::is_trivially_destructible_v<T>,
            "MemoryArena::create only supports trivially destructible types (destructors are not called on reset)");
        void* mem = allocate(sizeof(T), alignof(T));
        return ::new (mem) T(std::forward<Args>(args)...);
    }

    /// bulk reset bump pointers to zero across all blocks without deallocation
    /// inv: preserves allocated blocks for reuse, eliminating OS page faults
    /// out: arena is empty; all prior pointers are invalidated
    /// thread: not thread-safe
    /// err: noexcept
    void reset() noexcept {
        currentBlockIdx_ = 0;
        offset_ = 0;
    }

    /// release all backing OS memory blocks back to the system
    /// inv: frees all blocks and resets indices to zero
    /// thread: not thread-safe
    /// err: noexcept
    void clear() noexcept {
        for (const auto& block : blocks_) {
            if (block.ptr) {
#if defined(_MSC_VER) || defined(__MINGW32__)
                _aligned_free(block.ptr);
#else
                std::free(block.ptr);
#endif
            }
        }
        blocks_.clear();
        currentBlockIdx_ = 0;
        offset_ = 0;
    }

    // -- capacity and diagnostic metrics --------------------------------------

    /// total number of backing OS blocks allocated
    [[nodiscard]] std::size_t totalAllocatedBlocks() const noexcept { return blocks_.size(); }

    /// cumulative capacity across all backing blocks in bytes
    [[nodiscard]] std::size_t totalAllocatedCapacity() const noexcept {
        std::size_t sum = 0;
        for (const auto& b : blocks_)
            sum += b.capacity;
        return sum;
    }

    /// alias for totalAllocatedCapacity
    [[nodiscard]] std::size_t capacity() const noexcept { return totalAllocatedCapacity(); }

    /// 0-based index of the currently active block
    [[nodiscard]] std::size_t activeBlockIndex() const noexcept { return currentBlockIdx_; }

    /// byte offset within the currently active block
    [[nodiscard]] std::size_t currentOffset() const noexcept { return offset_; }

    /// total bytes currently consumed by allocations across blocks
    [[nodiscard]] std::size_t allocatedBytes() const noexcept {
        if (blocks_.empty()) return 0;
        std::size_t sum = 0;
        for (std::size_t i = 0; i < currentBlockIdx_; ++i) {
            sum += blocks_[i].capacity;
        }
        sum += offset_;
        return sum;
    }

private:
    std::vector<Block> blocks_;
    std::size_t currentBlockIdx_ = 0;
    std::size_t offset_ = 0;
    std::size_t defaultBlockSize_;
};

} // namespace razobill::core

#pragma once

struct OrderHot {
    unsigned long long price;
    unsigned int remaining_qty;
    unsigned char side;
    unsigned char _pad[3];
};

struct OrderCold {
    unsigned long long order_id;
    unsigned long long timestamp;
    unsigned int quantity;
    unsigned char type;
    unsigned char status;
    unsigned char _pad[10];
};

static_assert(sizeof(OrderHot) == 16, "OrderHot must be 16 bytes");
static_assert(sizeof(OrderCold) == 32, "OrderCold must be 32 bytes");

struct OrderHandler {
    unsigned int index;
    static constexpr unsigned int INVALID = 0xFFFFFFFFu;
    [[nodiscard]] bool valid() const noexcept { return index != INVALID; }
};

class PoolAllocator {
private:
    OrderHot* hot_ = nullptr;
    OrderCold* cold_ = nullptr;
    void* hot_alloc_ = nullptr;
    void* cold_alloc_ = nullptr;
    unsigned long long capacity_ = 0;
    unsigned long long size_ = 0;

    static void* alignedAlloc(unsigned long long bytes, unsigned long long alignment) noexcept {
        unsigned long long total = bytes + alignment - 1 + sizeof(void*);
        void* raw = ::operator new[](total);
        unsigned long long raw_addr = (unsigned long long) raw;
        unsigned long long offset = (alignment - ((raw_addr + sizeof(void*)) & (alignment - 1))) & (alignment - 1);
        unsigned long long aligned_addr = raw_addr + sizeof(void*) + offset;
        void** aligned_ptr = reinterpret_cast<void**>(aligned_addr);
        aligned_ptr[-1] = raw;
        return aligned_ptr;
    }

    static void alignedDealloc(void* ptr) noexcept {
        if (!ptr) return;
        void** aligned_ptr = reinterpret_cast<void**>(ptr);
        void* raw = aligned_ptr[-1];
        ::operator delete[](raw);
    }

    static void zeroBytes(void* ptr, unsigned long long n_bytes) noexcept {
        unsigned long long* p = static_cast<unsigned long long*>(ptr);
        unsigned long long count = n_bytes / sizeof(unsigned long long);
        for (unsigned long long i = 0; i < count; ++i) {
            p[i] = 0ULL;
        }
    }

public:
    explicit PoolAllocator(unsigned long long n_orders = 1024ull * 1024ull)
        : capacity_(n_orders)
    {
        unsigned long long hot_bytes = sizeof(OrderHot) * n_orders;
        unsigned long long cold_bytes = sizeof(OrderCold) * n_orders;
        hot_alloc_ = alignedAlloc(hot_bytes, 64ull);
        cold_alloc_ = alignedAlloc(cold_bytes, 64ull);
        hot_ = static_cast<OrderHot*>(hot_alloc_);
        cold_ = static_cast<OrderCold*>(cold_alloc_);
        zeroBytes(hot_, hot_bytes);
        zeroBytes(cold_, cold_bytes);
    }

    [[nodiscard]] OrderHandler allocate(const OrderHot hot, const OrderCold& cold) noexcept {
        if (size_ >= capacity_) return {OrderHandler::INVALID};
        unsigned int idx = static_cast<unsigned int>(size_);
        hot_[idx] = hot;
        cold_[idx] = cold;
        size_ += 1;
        return {idx};
    }

    [[nodiscard]] OrderHandler allocate_range(const OrderHot* hots, const OrderCold* colds, unsigned long long count) noexcept {
        if (count + size_ > capacity_) return {OrderHandler::INVALID};
        unsigned int base = static_cast<unsigned int>(size_);
        for (unsigned long long i = 0; i < count; ++i) {
            hot_[base + static_cast<unsigned int>(i)] = hots[i];
            cold_[base + static_cast<unsigned int>(i)] = colds[i];
        }
        size_ += count;
        return {base};
    }

    [[nodiscard]] OrderHot* getHot(const OrderHandler h) const noexcept {
        if (!h.valid() || static_cast<unsigned long long>(h.index) >= size_) return nullptr;
        return hot_ + h.index;
    }

    [[nodiscard]] OrderCold* getCold(const OrderHandler h) const noexcept {
        if (!h.valid() || static_cast<unsigned long long>(h.index) >= size_) return nullptr;
        return cold_ + h.index;
    }

    inline void reset() noexcept { size_ = 0ull; }

    void clear() noexcept {
        if (size_ == 0ull) return;
        zeroBytes(hot_, sizeof(OrderHot) * size_);
        zeroBytes(cold_, sizeof(OrderCold) * size_);
        size_ = 0ull;
    }

    [[nodiscard]] unsigned long long size() const noexcept { return size_; }
    [[nodiscard]] unsigned long long capacity() const noexcept { return capacity_; }

    ~PoolAllocator() {
        alignedDealloc(hot_alloc_);
        alignedDealloc(cold_alloc_);
    }

    PoolAllocator(const PoolAllocator&) = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;
};

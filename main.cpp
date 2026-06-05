#include <iostream>
#include <chrono>
#include <new>
#include <vector>

struct Order {
    uint64_t order_id;      // 8 bytes — unique ID
    uint64_t timestamp;     // 8 bytes — nanosecond timestamp
    uint64_t price;         // 8 bytes
    uint32_t quantity;      // 4 bytes
    uint32_t remaining_qty; // 4 bytes
    uint8_t  side;          // 1 byte — BID or ASK
    uint8_t  type;          // 1 byte — LIMIT, MARKET
    uint8_t  status;        // 1 byte — ACTIVE, CANCELLED, FILLED
    uint8_t  padding[5];    // 5 bytes — pad to 48 bytes total
};

static_assert(sizeof(Order) == 40, "Order size mismatch — recheck padding");
class PoolAllocator {
private:
    char *data_ = nullptr;
    size_t capacity_ = 0;
    size_t offset_ = 0;
public:
    explicit PoolAllocator(const size_t n_orders = 1024 * 1024) {
        capacity_ = sizeof(Order) * n_orders;
        // 64-byte aligned so the first slot lands on a cache-line boundary.
        data_ = static_cast<char*>(::operator new[](capacity_, std::align_val_t{64}));
    }
    [[nodiscard]]
    inline Order* allocate(const Order& order) noexcept {
        if (offset_ + sizeof(Order) > capacity_) return nullptr;
        auto* ptr = new (data_ + offset_) Order(order);
        offset_ += sizeof(Order);
        return ptr;
    }
    inline void reset() noexcept { offset_ = 0; }
    [[nodiscard]]
    inline size_t size() const noexcept{
        return offset_;
    }
    [[nodiscard]]
    inline size_t capacity() const noexcept {
        return capacity_;
    }
    ~PoolAllocator() {
        ::operator delete[](data_, std::align_val_t{64});
    }
    // remove the move and copy operator
    PoolAllocator(const PoolAllocator&)            = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;
};
static volatile uint64_t sink;
[[gnu::noinline]]
static void do_not_optimize(const uint64_t v) { sink = v; }

int main() {
    constexpr uint64_t N = 1'000'000;
    // we will use do_not_optimize and checksum to make sure the compiler doesn't optimize away the whole code
    {
        PoolAllocator pool(N);
        uint64_t checksum = 0;
        const auto t0 = std::chrono::high_resolution_clock::now();
        for (uint64_t i = 0; i < N; ++i) {
            const Order* o = pool.allocate({i, 1000, 102 + i, 45, 45, 1, 1, 1});
            checksum += o->order_id;
        }
        const auto t1 = std::chrono::high_resolution_clock::now();
        do_not_optimize(checksum);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        std::cout << "Pool allocator: " << ms << " ms  (" << us << " µs)\n";
    }
    {
        std::vector<Order*> ptrs;
        ptrs.reserve(N);
        uint64_t checksum = 0;
        const auto t0 = std::chrono::high_resolution_clock::now();
        for (uint64_t i = 0; i < N; ++i) {
            ptrs.push_back(new Order{i, 1000, 102 + i, 45, 45, 1, 1, 1});
            checksum += ptrs.back()->order_id;
        }
        for (const Order* p : ptrs) delete p;
        const auto t1 = std::chrono::high_resolution_clock::now();
        do_not_optimize(checksum);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        std::cout << "new / delete:" << ms << " ms  (" << us << " µs)\n";
    }
    return 0;
}
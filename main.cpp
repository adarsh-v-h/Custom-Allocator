#include <iostream>
#include <chrono>
#include <new>
#include <vector>
#include <cassert>
#include <cstring>
#include <span>
#include <immintrin.h> // for SIMD ops

struct Order {
    uint64_t order_id;      // 8 bytes — unique ID
    uint64_t timestamp;     // 8 bytes — nanosecond timestamp
    uint64_t price;         // 8 bytes
    uint32_t quantity;      // 4 bytes
    uint32_t remaining_qty; // 4 bytes
    uint8_t  side;          // 1 byte — BID or ASK
    uint8_t  type;          // 1 byte — LIMIT, MARKET
    uint8_t  status;        // 1 byte — ACTIVE, CANCELLED, FILLED
    uint8_t  _pad[5];    // 5 bytes — pad to 48 bytes total
};

// structure splitting
struct OrderHot {
    // OrderHot which will be called and used most often
    uint64_t price;
    uint32_t remaining_qty;
    uint8_t  side;
    uint8_t  _pad[3];
};

struct OrderCold {
    // less used data
    uint64_t order_id;
    uint64_t timestamp;
    uint32_t quantity;
    uint8_t  type;
    uint8_t  status;
    uint8_t  _pad[10];
};

// we need something to handle these orders, making sure they are properly being accessed, we will use index based accessing
struct OrderHandler {
    uint32_t index;
    static constexpr u_int32_t INVALID = UINT32_MAX; // making sure the index doesn't reach its max range value, even if it does, we don't process anything
    [[nodiscard]]
    bool valid() const noexcept { return index != INVALID; }
};

// The big guy
class PoolAllocator {
private:
    OrderHot* hot_ = nullptr;
    OrderCold* cold_ = nullptr;
    uint32_t capacity_ = 0;
    uint32_t size_ = 0;
    // this is a function which sets the whole allocated memory to 0, instead of just leaving it with garbage value
    // we will use intrinsic intel SIMD functions to get it done it in the most efficient way
    static void simdZero(void* ptr,const size_t n_bytes) noexcept {
        assert(n_bytes%32==0);
        const auto dst = static_cast<__m256i*>(ptr); // to cast the void pointer to a pointer that will target AVX register??
        const __m256i zero = _mm256_setzero_si256(); // setting a 32 byte register to 0, which we will use to set other values as 0
        const size_t n = n_bytes/32; // no of interactions
        for (size_t i = 0; i < n; ++i) {
            _mm256_store_si256(dst+i, zero); // aligned store, i.e 32 bytes
        }
    }
    // this function also uses SIMD, but it is to copy from 1 source to another
    static void simdCopy(void* dst, const void* src,const size_t n_bytes) noexcept {
        assert(n_bytes%32==0);
        auto* d = static_cast<__m256i*>(dst);
        const auto* s = static_cast<const __m256i*>(src);
        const size_t n = n_bytes/32;
        for (size_t i = 0; i < n; ++i) {
            _mm256_store_si256(d+i, _mm256_loadu_si256(s+i));
        }
    }
public:
    explicit PoolAllocator(const size_t n_orders = 1024 * 1024) :capacity_(n_orders) {
        // 64-byte aligned so the first slot lands on a cache-line boundary.
        hot_ = static_cast<OrderHot*>(::operator new[](sizeof(OrderHot) * n_orders, std::align_val_t{64}));
        cold_ = static_cast<OrderCold*>(::operator new[](sizeof(OrderCold) * n_orders, std::align_val_t{64}));
        // now we will zero out both pools
        // but for it work, the data must be a divisble of 32, for strutCold its not an issue, since its size itself is 32
        // for OrderHot which is of 16 bytes, the number of orders must be even for this to work
        assert(n_orders % 2 == 0); // we will just check if the last bit of the value is 0, if so its even, so OrderHot will be fine
        simdZero(hot_,n_orders*sizeof(OrderHot));
        simdZero(cold_,n_orders*sizeof(OrderCold));
    }
    // Single allocation
    [[nodiscard]]
    inline OrderHandler allocate(const OrderHot hot, const OrderCold& cold) noexcept {
        // check if the size went over the capacity
        if (size_ >= capacity_) return {OrderHandler::INVALID};
        const auto idx = size_++; // get the existing empty idx
        new(hot_+idx) OrderHot{hot};
        new(cold_+idx) OrderCold{cold};
        return {idx}; // implicit conversion
    }
    // Bulk Allocation
    [[nodiscard]]
    OrderHandler allocate_range(std::span<const OrderHot> hots, std::span<const OrderCold> colds) {
        // we use std::span when we don't want to pass a whole bunch of arrays of struct, so we pass them as std::span
        // which actually is like passing a pointer and a number of elements
        // but we need to make sure
        assert(hots.size() == colds.size());
        // figure out how many we want to allcoate
        const auto count = static_cast<uint32_t>(hots.size());
        // check if we even have such level of space
        if (count+size_ > capacity_) return {OrderHandler::INVALID};
        const auto base = size_;
        size_ += count;
        // first hot order, using AVX2 which is the only one supported by my processor
        // we are gonna process in pair, since we can using AVX, man it feels like a super power
        {
            const auto src = reinterpret_cast<const __m256i*>(hots.data());
            const auto dst = reinterpret_cast<__m256i*>(hot_+base);
            // moving data that is held by hots's span(data() function returns raw pointer to underlying array of struct)
            // now first lets move the first even order, that is if odd we will have 1 remaining, or else 0 if even
            const uint32_t paris = count/2;
            for (uint32_t i = 0; i < paris; ++i) {
                _mm256_store_si256(dst+i, _mm256_loadu_si256(src+i));
            }
            // now check if the numbers we odd, using bitwise op
            if (count & 1) {
                // we will do this one the OG manner
                new(hot_ + base + count - 1) OrderHot(hots[count-1]);
            }
        }
        // Now something similar to cold orders, and since they are bothered by odd or even number of counts no extra checking needed
        {
            const auto src = reinterpret_cast<const __m256i*>(colds.data());
            const auto dst = reinterpret_cast<__m256i*>(cold_+base);
            for (uint32_t i = 0;i<count;++i) {
                _mm256_store_si256(dst+i, _mm256_load_si256(src+i));
            }
        }
        return {base};
    }
    // To access the hot order details
    [[nodiscard]]
    inline OrderHot* getHot(const OrderHandler h) const noexcept {
        assert(h.valid() && h.index < size_); // making sure that Orderhandler is valid
        return (hot_ + h.index);
    }
    // to just access the cold details
    [[nodiscard]]
    inline OrderCold* getCold(const OrderHandler h) const noexcept {
        assert(h.valid() && h.index < size_);
        return (cold_ + h.index);
    }
    inline void reset() noexcept { size_ = 0; } // sets the start index to 0
    // the data is still there, but marking them as empty, and further modifying as we go on
    // but what if you want a clean slate, you use clear:
    void clear() noexcept {
        if (size_==0) return;
        // now for setting 0 we will again use AVX function which we created above
        // but for that hot must be even again, so a layer of checking
        const size_t hot_b = sizeof(OrderHot) * size_;
        const size_t hot_simd = (hot_b/32) * 32; // simple
        simdZero(hot_,hot_simd);
        if (hot_b > hot_simd) {
            memset(reinterpret_cast<char*>(hot_)+hot_simd,0,hot_b-hot_simd);
        }
        simdZero(cold_,sizeof(OrderCold) * size_);
        size_ = 0;
    }
    [[nodiscard]]
    inline size_t size() const noexcept{ return size_; }
    [[nodiscard]]
    inline size_t capacity() const noexcept { return capacity_; }
    ~PoolAllocator() {
        ::operator delete[](hot_, std::align_val_t{64});
        ::operator delete[](cold_, std::align_val_t{64});
    }
    // remove the move and copy operator
    PoolAllocator(const PoolAllocator&)            = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;
};
static volatile uint64_t sink;
[[gnu::noinline]]
static void do_not_optimize(const uint64_t v) { sink = v; }
// [[gnu::noinline]] is to make sure the compiler doesn't inline this, whatever optimization flag you use

int main() {
    constexpr uint64_t N = 1'000'000;
    // we will use do_not_optimize and checksum to make sure the compiler doesn't optimize away the whole code
    {
        PoolAllocator pool(N);
        std::vector<OrderHandler> handles;
        handles.reserve(N); // so that we don't call shitty allocator for this all the time now
        const auto t0 = std::chrono::high_resolution_clock::now();
        for (uint32_t i = 0; i < N; ++i) {
            handles.emplace_back(pool.allocate(
                OrderHot  {102 + i, 45, 1, {}},
                OrderCold {i, 1000, 45, 1, 0, {}}
            ));
        }
        const auto t1 = std::chrono::high_resolution_clock::now();
        std::cout << "Allocation of " << N << " orders: "
              << std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()
              << "µs\n";
        // now only hot access
        uint64_t checksum = 0;
        const auto t2 = std::chrono::high_resolution_clock::now();
        for (uint32_t i = 0; i < N; ++i) {
            checksum += pool.getHot(handles[i])->price;
        }
        const auto t3 = std::chrono::high_resolution_clock::now();
        do_not_optimize(checksum);
        std::cout << "Hot-only scan: "
                  << std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count()
                  << "µs\n";
        // now only cold access
        const auto t4 = std::chrono::high_resolution_clock::now();
        for (uint32_t i = 0; i < N; ++i) {
            checksum += pool.getCold(handles[i])->order_id;
        }
        const auto t5 = std::chrono::high_resolution_clock::now();
        do_not_optimize(checksum);
        std::cout << "Cold scan: "
                << std::chrono::duration_cast<std::chrono::microseconds>(t5 - t4).count()
                <<"μs\n";
        // Some math to understand how much is what
        std::cout << "\nWorking set(hot " << N << " orders): "
          << (sizeof(OrderHot) * N) / 1024 << "KB\n";
        std::cout << "Working set(cold " << N << " orders): "
                  << (sizeof(OrderCold) * N) / 1024 << "KB\n";
        std::cout << "Hot orders per cache line: "
                  << 64 / sizeof(OrderHot) << "\n\n";
    }
    {
        std::vector<Order*> ptrs;
        ptrs.reserve(N);
        uint64_t checksum = 0;
        const auto t0 = std::chrono::high_resolution_clock::now();
        for (uint64_t i = 0; i < N; ++i) {
            ptrs.push_back(new Order{i, 1000, 102 + i, 45, 45, 1, 1, 1, {}});
            checksum += ptrs.back()->order_id;
        }
        for (const Order* p : ptrs) delete p;
        const auto t1 = std::chrono::high_resolution_clock::now();
        do_not_optimize(checksum);
        std::cout << "new/delete: " <<
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()
            <<"µs\n";
        // Now access time for regular order with regular method
        std::vector<Order*> ptrs2;
        ptrs2.reserve(N);
        for (uint64_t i = 0; i < N; ++i) {
            ptrs2.push_back(new Order{i, 1000, 102 + i, 45, 45, 1, 1, 1, {}});
        }
        const auto t2 = std::chrono::high_resolution_clock::now();
        checksum = 0;
        for (uint64_t i = 0; i < N; ++i) {
            checksum += ptrs[i]->order_id;
        }
        const auto t3 = std::chrono::high_resolution_clock::now();
        do_not_optimize(checksum);
        std::cout << "Order access time: "
        << std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count()
        <<"µs\n";
        for (const Order* p : ptrs2) delete p;
    }
    return 0;
}
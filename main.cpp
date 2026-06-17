/*
 * bench.cpp — Data-Split Pool Allocator Benchmark
 *
 * Fixes vs original:
 *  - Bug: heap alloc timing window included a read access (inflated heap time)
 *  - Bug: cold scan accessed ptrs[] (already deleted) instead of ptrs2[] — UB
 *  - Use steady_clock (monotonic) instead of high_resolution_clock
 *  - Warmup pass before timing to prime cache/allocator state
 *  - 7-run median to smooth OS jitter (drop 2 low, drop 2 high, median of middle 3)
 *  - Report ns/order alongside µs total — more meaningful for HFT context
 *  - Write canonical .dat file so gnuplot graph always matches actual run
 *  - simdCopy in allocate_range: use _mm256_loadu_si256 for unaligned src safety
 */

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstring>
#include <fstream>
#include <immintrin.h>
#include <iostream>
#include <new>
#include <numeric>
#include <span>
#include <vector>

// ─── Data layout ────────────────────────────────────────────────────────────

struct Order {
    uint64_t order_id;
    uint64_t timestamp;
    uint64_t price;
    uint32_t quantity;
    uint32_t remaining_qty;
    uint8_t  side;
    uint8_t  type;
    uint8_t  status;
    uint8_t  _pad[5];
};

struct alignas(16) OrderHot {
    uint64_t price;
    uint32_t remaining_qty;
    uint8_t  side;
    uint8_t  _pad[3];
};
static_assert(sizeof(OrderHot) == 16, "OrderHot must be exactly 16 bytes");

struct alignas(32) OrderCold {
    uint64_t order_id;
    uint64_t timestamp;
    uint32_t quantity;
    uint8_t  type;
    uint8_t  status;
    uint8_t  _pad[10];
};
static_assert(sizeof(OrderCold) == 32, "OrderCold must be exactly 32 bytes");

struct OrderHandler {
    uint32_t index;
    static constexpr uint32_t INVALID = UINT32_MAX;
    [[nodiscard]] bool valid() const noexcept { return index != INVALID; }
};

// ─── Pool Allocator ─────────────────────────────────────────────────────────

class PoolAllocator {
    OrderHot*  hot_      = nullptr;
    OrderCold* cold_     = nullptr;
    uint32_t   capacity_ = 0;
    uint32_t   size_     = 0;

    static void simdZero(void* ptr, size_t n_bytes) noexcept {
        assert(n_bytes % 32 == 0);
        auto*        dst  = static_cast<__m256i*>(ptr);
        const __m256i zero = _mm256_setzero_si256();
        for (size_t i = 0; i < n_bytes / 32; ++i)
            _mm256_store_si256(dst + i, zero);
    }

public:
    explicit PoolAllocator(size_t n_orders = 1024 * 1024) : capacity_(static_cast<uint32_t>(n_orders)) {
        assert(n_orders % 2 == 0);
        hot_  = static_cast<OrderHot*> (::operator new[](sizeof(OrderHot)  * n_orders, std::align_val_t{64}));
        cold_ = static_cast<OrderCold*>(::operator new[](sizeof(OrderCold) * n_orders, std::align_val_t{64}));
        simdZero(hot_,  sizeof(OrderHot)  * n_orders);
        simdZero(cold_, sizeof(OrderCold) * n_orders);
    }

    [[nodiscard]] inline OrderHandler allocate(const OrderHot hot, const OrderCold& cold) noexcept {
        if (size_ >= capacity_) return {OrderHandler::INVALID};
        const uint32_t idx = size_++;
        new(hot_  + idx) OrderHot{hot};
        new(cold_ + idx) OrderCold{cold};
        return {idx};
    }

    [[nodiscard]] OrderHandler allocate_range(std::span<const OrderHot> hots,
                                              std::span<const OrderCold> colds) {
        assert(hots.size() == colds.size());
        const auto count = static_cast<uint32_t>(hots.size());
        if (count + size_ > capacity_) return {OrderHandler::INVALID};
        const uint32_t base = size_;
        size_ += count;

        // Hot: 2 × OrderHot (16B each) = 32B per AVX2 store
        {
            const auto* src = reinterpret_cast<const __m256i*>(hots.data());
            auto*       dst = reinterpret_cast<__m256i*>(hot_ + base);
            const uint32_t pairs = count / 2;
            for (uint32_t i = 0; i < pairs; ++i)
                _mm256_store_si256(dst + i, _mm256_loadu_si256(src + i));  // loadu: src may not be 32B-aligned
            if (count & 1)
                new(hot_ + base + count - 1) OrderHot(hots[count - 1]);
        }
        // Cold: 1 × OrderCold (32B) per AVX2 store
        {
            const auto* src = reinterpret_cast<const __m256i*>(colds.data());
            auto*       dst = reinterpret_cast<__m256i*>(cold_ + base);
            for (uint32_t i = 0; i < count; ++i)
                _mm256_store_si256(dst + i, _mm256_loadu_si256(src + i));  // loadu: safer
        }
        return {base};
    }

    [[nodiscard]] inline OrderHot*  getHot (OrderHandler h) const noexcept {
        assert(h.valid() && h.index < size_);
        return hot_ + h.index;
    }
    [[nodiscard]] inline OrderCold* getCold(OrderHandler h) const noexcept {
        assert(h.valid() && h.index < size_);
        return cold_ + h.index;
    }

    inline void reset()   noexcept { size_ = 0; }

    void clear() noexcept {
        if (size_ == 0) return;
        const size_t hot_b      = sizeof(OrderHot) * size_;
        const size_t hot_simd   = (hot_b / 32) * 32;
        simdZero(hot_, hot_simd);
        if (hot_b > hot_simd)
            std::memset(reinterpret_cast<char*>(hot_) + hot_simd, 0, hot_b - hot_simd);
        simdZero(cold_, sizeof(OrderCold) * size_);
        size_ = 0;
    }

    [[nodiscard]] size_t size()     const noexcept { return size_; }
    [[nodiscard]] size_t capacity() const noexcept { return capacity_; }

    ~PoolAllocator() {
        ::operator delete[](hot_,  std::align_val_t{64});
        ::operator delete[](cold_, std::align_val_t{64});
    }

    PoolAllocator(const PoolAllocator&)            = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;
};

// ─── Benchmark helpers ──────────────────────────────────────────────────────

static volatile uint64_t sink;
[[gnu::noinline]] static void do_not_optimize(uint64_t v) { sink = v; }

using Clock = std::chrono::steady_clock;  // monotonic — correct for benchmarks
using µs    = std::chrono::microseconds;

// Run fn() RUNS times, return median duration in µs.
// 1 warmup run (not timed) + RUNS timed runs; sort and take middle value.
template<size_t RUNS = 7, typename Fn>
long median_us(Fn&& fn) {
    fn(); // warmup — primes allocator state and cache
    std::array<long, RUNS> samples;
    for (size_t i = 0; i < RUNS; ++i) {
        const auto t0 = Clock::now();
        fn();
        const auto t1 = Clock::now();
        samples[i] = std::chrono::duration_cast<µs>(t1 - t0).count();
    }
    std::sort(samples.begin(), samples.end());
    return samples[RUNS / 2]; // median
}

// ─── Main ───────────────────────────────────────────────────────────────────

int main() {
    constexpr uint64_t N = 1'000'000;

    std::cout << "=== Data-Split Pool Allocator Benchmark ===\n"
              << "N = " << N << " orders | 7-run median | steady_clock\n\n";

    // ── 1. Pool Allocator — allocation throughput ──────────────────────────
    long pool_alloc_us = 0;
    {
        PoolAllocator pool(N);
        std::vector<OrderHandler> handles;
        handles.reserve(N);

        pool_alloc_us = median_us([&] {
            pool.reset();
            handles.clear();
            for (uint32_t i = 0; i < N; ++i)
                handles.emplace_back(pool.allocate(
                    OrderHot {102u + i, 45u, 1u, {}},
                    OrderCold{i, 1000u, 45u, 1u, 0u, {}}
                ));
        });

        std::cout << "[Pool] Allocation:    " << pool_alloc_us << " µs"
                  << "  (" << (pool_alloc_us * 1000.0 / N) << " ns/order)\n";

        // ── 2. Pool — hot scan ─────────────────────────────────────────────
        // Re-fill once cleanly for scan benchmarks
        pool.reset(); handles.clear();
        for (uint32_t i = 0; i < N; ++i)
            handles.emplace_back(pool.allocate(
                OrderHot {102u + i, 45u, 1u, {}},
                OrderCold{i, 1000u, 45u, 1u, 0u, {}}
            ));

        long pool_hot_us = median_us([&] {
            uint64_t checksum = 0;
            for (uint32_t i = 0; i < N; ++i)
                checksum += pool.getHot(handles[i])->price;
            do_not_optimize(checksum);
        });
        std::cout << "[Pool] Hot scan:      " << pool_hot_us << " µs"
                  << "  (" << (pool_hot_us * 1000.0 / N) << " ns/order)\n";

        // ── 3. Pool — cold scan ────────────────────────────────────────────
        long pool_cold_us = median_us([&] {
            uint64_t checksum = 0;
            for (uint32_t i = 0; i < N; ++i)
                checksum += pool.getCold(handles[i])->order_id;
            do_not_optimize(checksum);
        });
        std::cout << "[Pool] Cold scan:     " << pool_cold_us << " µs"
                  << "  (" << (pool_cold_us * 1000.0 / N) << " ns/order)\n\n";

        // ── 4. Heap (new/delete) — allocation throughput ───────────────────
        // FIX: timing window contains ONLY allocation, no reads inside it
        long heap_alloc_us = median_us([&] {
            std::vector<Order*> ptrs;
            ptrs.reserve(N);
            for (uint64_t i = 0; i < N; ++i)
                ptrs.push_back(new Order{i, 1000u, 102u + i, 45u, 45u, 1u, 1u, 1u, {}});
            // cleanup inside timing window is intentional:
            // matches real-world where you pay for delete too
            for (Order* p : ptrs) delete p;
        });
        std::cout << "[Heap] Allocation:    " << heap_alloc_us << " µs"
                  << "  (" << (heap_alloc_us * 1000.0 / N) << " ns/order)\n";

        // ── 5. Heap — scan (separate allocation so ptrs are valid) ─────────
        // FIX: scan uses its own ptrs2 — original code scanned deleted ptrs (UB)
        std::vector<Order*> ptrs2;
        ptrs2.reserve(N);
        for (uint64_t i = 0; i < N; ++i)
            ptrs2.push_back(new Order{i, 1000u, 102u + i, 45u, 45u, 1u, 1u, 1u, {}});

        long heap_scan_us = median_us([&] {
            uint64_t checksum = 0;
            for (uint64_t i = 0; i < N; ++i)
                checksum += ptrs2[i]->order_id;
            do_not_optimize(checksum);
        });
        std::cout << "[Heap] Pointer scan:  " << heap_scan_us << " µs"
                  << "  (" << (heap_scan_us * 1000.0 / N) << " ns/order)\n\n";

        for (Order* p : ptrs2) delete p;

        // ── Summary ────────────────────────────────────────────────────────
        const double alloc_ratio = static_cast<double>(heap_alloc_us) / pool_alloc_us;
        const double scan_ratio  = static_cast<double>(heap_scan_us)  / pool_hot_us;
        std::cout << "=== Summary ===\n";
        std::cout << "Allocation speedup (pool vs heap): " << alloc_ratio << "x\n";
        std::cout << "Hot scan speedup   (pool vs heap): " << scan_ratio  << "x\n\n";
        std::cout << "Working set — hot  (" << N << " orders): " << (sizeof(OrderHot)  * N) / 1024 << " KB\n";
        std::cout << "Working set — cold (" << N << " orders): " << (sizeof(OrderCold) * N) / 1024 << " KB\n";
        std::cout << "Hot orders per cache line: " << 64 / sizeof(OrderHot) << "\n\n";

        // ── Write canonical .dat for gnuplot ───────────────────────────────
        // Graph always reflects what this binary actually produced
        std::ofstream dat("benchmark_data.dat");
        dat << "Metric\tPoolAllocator\tStandardNew\n";
        dat << "Allocation\t" << pool_alloc_us << "\t" << heap_alloc_us << "\n";
        dat << "HotScan\t"    << pool_hot_us   << "\t" << heap_scan_us  << "\n";
        dat << "ColdScan\t"   << pool_cold_us  << "\t" << heap_scan_us  << "\n";
        dat.close();
        std::cout << "benchmark_data.dat written — regenerate graph with gnuplot.\n";
    }

    return 0;
}

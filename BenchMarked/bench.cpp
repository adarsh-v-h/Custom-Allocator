#include <chrono>
#include <cstdint>
#include <iostream>
#include <new>
#include <vector>

#include "SplitPoolAllocator.h"

struct Order {
    uint64_t order_id;
    uint64_t timestamp;
    uint64_t price;
    uint32_t quantity;
    uint32_t remaining_qty;
    uint8_t side;
    uint8_t type;
    uint8_t status;
    uint8_t _pad[5];
};

static volatile uint64_t sink;
[[gnu::noinline]] static void do_not_optimize(const uint64_t v) {
    sink = v;
}

int main() {
    constexpr uint64_t N = 1'000'000;

    {
        PoolAllocator pool(N);
        std::vector<OrderHandler> handles;
        handles.reserve(N);

        const auto t0 = std::chrono::high_resolution_clock::now();
        for (uint64_t i = 0; i < N; ++i) {
            handles.emplace_back(pool.allocate(
                OrderHot{102ull + i, 45u, 1u, {}},
                OrderCold{i, 1000ull, 45u, 1u, 0u, {}}
            ));
        }
        const auto t1 = std::chrono::high_resolution_clock::now();
        std::cout << "Allocation of " << N << " orders: "
                  << std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()
                  << "µs\n";

        uint64_t checksum = 0;
        const auto t2 = std::chrono::high_resolution_clock::now();
        for (uint64_t i = 0; i < N; ++i) {
            checksum += pool.getHot(handles[i])->price;
        }
        const auto t3 = std::chrono::high_resolution_clock::now();
        do_not_optimize(checksum);
        std::cout << "Hot-only scan: "
                  << std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count()
                  << "µs\n";

        const auto t4 = std::chrono::high_resolution_clock::now();
        for (uint64_t i = 0; i < N; ++i) {
            checksum += pool.getCold(handles[i])->order_id;
        }
        const auto t5 = std::chrono::high_resolution_clock::now();
        do_not_optimize(checksum);
        std::cout << "Cold scan: "
                  << std::chrono::duration_cast<std::chrono::microseconds>(t5 - t4).count()
                  << "µs\n";

        std::cout << "\nWorking set (hot " << N << " orders): "
                  << (sizeof(OrderHot) * N) / 1024 << " KB\n";
        std::cout << "Working set (cold " << N << " orders): "
                  << (sizeof(OrderCold) * N) / 1024 << " KB\n";
        std::cout << "Hot orders per cache line: "
                  << 64 / sizeof(OrderHot) << "\n\n";
    }

    {
        std::vector<Order*> ptrs;
        ptrs.reserve(N);
        uint64_t checksum = 0;
        const auto t0 = std::chrono::high_resolution_clock::now();
        for (uint64_t i = 0; i < N; ++i) {
            ptrs.push_back(new Order{i, 1000ull, 102ull + i, 45u, 45u, 1u, 1u, 1u, {}});
        }
        const auto t1 = std::chrono::high_resolution_clock::now();
        const auto alloc_time = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        do_not_optimize(alloc_time);
        std::cout << "new/delete allocation: "
                  << alloc_time
                  << "µs\n";

        const auto t2 = std::chrono::high_resolution_clock::now();
        checksum = 0;
        for (uint64_t i = 0; i < N; ++i) {
            checksum += ptrs[i]->order_id;
        }
        const auto t3 = std::chrono::high_resolution_clock::now();
        const auto cold_access_time = std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count();
        do_not_optimize(checksum);
        std::cout << "Standard pointer cold scan: "
                  << cold_access_time
                  << "µs\n";

        for (Order* p : ptrs) delete p;
    }

    return 0;
}

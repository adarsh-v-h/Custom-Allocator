## What actually happens in an HFT system at the memory level

**The data characteristics:**

In a real exchange feed, you're receiving market data — orders, cancellations, trades — arriving over UDP multicast at rates of **1-10 million messages per second** on a busy day. Each message is **small** — a typical order entry is 40-100 bytes. It arrives, gets processed in nanoseconds, and then most of it becomes irrelevant almost immediately.

So the access pattern looks like this:
- Allocate small fixed-size chunks, extremely frequently
- Hold them for a very short time (microseconds to milliseconds)
- Deallocate in roughly the order they were allocated (FIFO-ish)
- **Never** block. Ever. `malloc` is disqualified — it takes locks internally, has unpredictable latency, and fragments over time.
- **No** garbage collection pause. Ever.
- Cache misses are death — if your allocator scatters objects across memory, the CPU spends more time fetching cache lines than doing work.

**What the order book itself needs from memory:**

An order book maintains two sides — bids and asks — each a sorted structure of price levels, each price level containing a queue of orders at that price. Operations are:
- Add order → allocate an Order object, insert into price level
- Cancel order → find it, deallocate, remove
- Match order → consume from front of queue, deallocate

Key insight: **Order objects are all the same size.** This is not like a general-purpose allocator that handles arbitrary sizes. You're allocating the same struct, thousands of times per second, and freeing them in no particular order.

---

## What your custom allocator needs to do

Given the above, here's what we're actually building:

**1. Pool Allocator (core)**
Pre-allocate a large contiguous block of memory upfront — say 64MB. Carve it into fixed-size slots equal to `sizeof(Order)`. Hand out slots in O(1). Take them back in O(1). No `malloc`, no `free`, no fragmentation ever.

**2. Free list**
When a slot is returned, push it onto a singly linked list of free slots. Next allocation pops from this list. Both operations are O(1) and branchless if done right.

**3. Cache-line alignment**
Each Order object should be aligned to 64 bytes (one cache line) so that accessing an order never straddles two cache lines — which would double the memory fetch cost.

**4. No-throw guarantee**
If the pool is exhausted, you don't call `malloc` as fallback. You either return null or have a secondary pool. You never block, never throw, never call into the OS.

**5. Benchmarkable**
The whole point is speed. So it needs a benchmark — allocation throughput (allocs per second), latency per alloc (nanoseconds), comparison against `new`/`delete` and against `malloc`/`free`.

---

## What we are NOT building (scope control)

- Not a general-purpose allocator (different sizes, arbitrary lifetimes)
- Not a thread-safe allocator yet (that's a v2 — lock-free free list with atomics)
- Not a garbage collector
- Not integrated with the order book yet — that's v3

---

## The struct we're allocating for

Before writing a single line of allocator code, you need to define what an Order looks like in memory:

```cpp
struct Order {
    uint64_t order_id;      // 8 bytes — unique ID
    uint64_t timestamp;     // 8 bytes — nanosecond timestamp
    uint64_t   price;         // 8 bytes
    uint32_t quantity;      // 4 bytes
    uint32_t remaining_qty; // 4 bytes
    uint8_t  side;          // 1 byte — BID or ASK
    uint8_t  type;          // 1 byte — LIMIT, MARKET
    uint8_t  status;        // 1 byte — ACTIVE, CANCELLED, FILLED
    uint8_t  padding[5];    // 5 bytes — pad to 48 bytes total
};
// Total: 40 bytes. Pad to 64 for cache-line alignment.
```

That struct definition is your starting point — before allocator, before order book, before anything. Get that right first.

---

## Build sequence

**Step 1:** Define the Order struct. Think about every field. Is `double` right for price or should it be a fixed-point integer to avoid floating point issues? (Hint: HFT systems almost always use fixed-point.)

**Step 2:** Write the PoolAllocator class. Fixed block, free list, `allocate()` and `deallocate()` only.

**Step 3:** Write the benchmark. Allocate 1 million orders, deallocate them, measure total time and per-operation latency. Compare against `new`/`delete`.

**Step 4:** Add cache-line alignment. Re-run benchmark. Document the difference.

**Step 5:** Write a short design doc — what decisions you made, why, and what the numbers showed.

That's v1. Clean, scoped, benchmarkable, and directly relevant to everything that comes after.

/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: GPLv2 + runtime exception (see LICENSE).
 */

/**
 * @file bench/bench_allocator.cpp
 * @brief This allocator against the system one, in the patterns that matter.
 *
 * Four patterns, because an allocator that wins at one can lose at another:
 *
 *  1. **Bursts of small blocks freed together.**  The compiler's usual shape,
 *     and where the size classes earn their keep.
 *  2. **Large blocks.**  Served by spans of chunks since 2026-09; before that
 *     they went to the system, at about 1.13 us per allocate/free pair.
 *  3. **Many threads at once.**  The point of per-thread free lists: the fast
 *     path synchronises NOTHING, so this should scale roughly with the cores.
 *  4. **Freed by a DIFFERENT thread than allocated it.**  The case that sinks
 *     naive per-thread allocators.  Here it costs one compare-exchange and
 *     blocks nobody.
 *
 * Run the same binary with VESTA_NO_HOST_SLAB=1 to compare against the system
 * allocator with everything else held constant.  That switch exists for exactly
 * this: without a way to turn it off there is nothing to compare against, and
 * then there is no way to know whether an allocator improves anything.
 */

#include "util/host_allocator.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

/// Sizes chosen from a real measurement: 81% of allocations are <= 64 bytes.
const size_t kSmall[] = {16, 24, 32, 32, 48, 64, 64, 96, 128, 256};
const int kSmallCount = int(sizeof(kSmall) / sizeof(kSmall[0]));

double ns_per_op(const char *name, long long ops,
                 std::chrono::steady_clock::time_point t0) {
    const auto dt = std::chrono::steady_clock::now() - t0;
    const double ns =
        double(std::chrono::duration_cast<std::chrono::nanoseconds>(dt)
                   .count()) /
        double(ops);
    std::printf("  %-38s %7.2f ns/op\n", name, ns);
    return ns;
}

/// 1. Bursts: allocate a batch, free the batch.
void bench_bursts() {
    const int batch = 512, rounds = 4000;
    // The extra parens are not decoration: `std::vector<void *> live(size_t(n))`
    // parses as a function declaration, and then the assignments below fail
    // with an error that says nothing about the real cause.
    std::vector<void *> live(static_cast<size_t>(batch));
    const auto t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < rounds; ++r) {
        for (int i = 0; i < batch; ++i)
            live[size_t(i)] = util::host_alloc(kSmall[i % kSmallCount]);
        for (int i = 0; i < batch; ++i)
            util::host_free(live[size_t(i)]);
    }
    ns_per_op("small, allocated and freed in bursts",
              2LL * batch * rounds, t0);
}

/// 2. Large blocks, across the whole measured tail.
void bench_large() {
    const size_t sizes[] = {4096, 40000, 200000, 1u << 20};
    const int n = int(sizeof(sizes) / sizeof(sizes[0]));
    const int rounds = 4000;
    const auto t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < rounds; ++r)
        for (int i = 0; i < n; ++i)
            util::host_free(util::host_alloc(sizes[i]));
    ns_per_op("large (4K..1M), allocate and free", 2LL * n * rounds, t0);
}

/// 3. Several threads, each minding its own business.
void bench_threads(unsigned threads) {
    const int per_thread = 200000;
    std::vector<std::thread> pool;
    const auto t0 = std::chrono::steady_clock::now();
    for (unsigned t = 0; t < threads; ++t)
        pool.emplace_back([per_thread] {
            for (int i = 0; i < per_thread; ++i)
                util::host_free(
                    util::host_alloc(kSmall[i % kSmallCount]));
        });
    for (auto &t : pool)
        t.join();
    char name[80];
    std::snprintf(name, sizeof(name), "small, %u threads, each on its own",
                  threads);
    ns_per_op(name, 2LL * per_thread * threads, t0);
}

/// 4. One thread allocates, another frees.  The hard case.
void bench_cross_thread() {
    const int n = 200000;
    std::vector<void *> blocks(static_cast<size_t>(n));
    const auto t0 = std::chrono::steady_clock::now();

    std::thread producer([&] {
        for (int i = 0; i < n; ++i)
            blocks[size_t(i)] = util::host_alloc(kSmall[i % kSmallCount]);
    });
    producer.join();

    std::thread consumer([&] {
        for (int i = 0; i < n; ++i)
            util::host_free(blocks[size_t(i)]);
    });
    consumer.join();

    ns_per_op("small, allocated here and freed THERE", 2LL * n, t0);
}

} // namespace

int main() {
    std::printf("== allocator benchmark ==\n\n");
    std::printf("allocator: %s\n\n",
                util::host_alloc_active()
                    ? "this one"
                    : "the SYSTEM one (VESTA_NO_HOST_SLAB is set)");

    // Warm up: the first allocation of a thread registers it and asks the OS
    // for a chunk.  Timing that would measure startup, not steady state.
    for (int i = 0; i < 10000; ++i)
        util::host_free(util::host_alloc(64));

    bench_bursts();
    bench_large();
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    bench_threads(1);
    bench_threads(hw / 2 > 1 ? hw / 2 : 2);
    bench_threads(hw);
    bench_cross_thread();

    const util::HostAllocStats s = util::host_alloc_stats();
    std::printf("\ncommitted %.1f MiB, %llu chunks, %llu freed by another "
                "thread\n",
                s.bytes_reserved / (1024.0 * 1024.0),
                (unsigned long long)s.chunks,
                (unsigned long long)s.remote_frees);
    std::printf("\nCompare by running this same binary with "
                "VESTA_NO_HOST_SLAB=1.\n");
    return 0;
}

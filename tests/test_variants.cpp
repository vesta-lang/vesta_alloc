/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file tests/test_variants.cpp
 * @brief One battery, every specialisation.  They are SPECIALISATIONS, not
 *        different allocators, so what has to hold is that they behave alike.
 *
 * There are three ways in, and picking one is a choice of TYPE resolved at
 * compile time -- never a switch the program can flip, because a "no threads
 * here" flag corrupts silently the day something starts a thread:
 *
 *   - the general one, `host_alloc`, which serves the whole process and bounds
 *     MEMORY: past `kMaxThreads` owners the rest share one cache behind a lock;
 *   - @c PerThreadAllocator, which bounds LATENCY instead: a cache for every
 *     thread and no lock at all, paid for in memory;
 *   - @c SingleOwnerAllocator, for something with a declared owner: one fixed
 *     cache, no thread slot to read and no owner check when freeing.
 *
 * WHY ONE FILE AND ONE BATTERY.  Because the property that matters is shared:
 * whatever comes out of any of them has to be interchangeable with the others.
 * Written as a test per variant, each one grows its own idea of what to check,
 * and the day a fourth appears the gaps are invisible.  Here a variant is a
 * ROW, and adding one means it is immediately held to everything the others
 * already pass.
 *
 * What the common battery checks, for every variant:
 *
 *  1. It serves, aligned and writable, and two live blocks never overlap.
 *  2. Its blocks and the general allocator's are interchangeable, both ways.
 *  3. Frees are LOCAL in BOTH regions -- the small classes and the big ones
 *     past `kBigClassMin`.  That second half is not a detail: those blocks live
 *     in another region, so a free that only asks `in_region` leaves through
 *     the general path, where the owner is re-derived from a thread slot that
 *     never matches, and every single one turns into an atomic.
 *  4. What ANOTHER thread releases comes back and is reused.
 *
 * And then what only makes sense per variant: that a single owner counts in its
 * own store and two of them do not collide, and that the per-thread one really
 * gives every thread a cache when there are more threads alive than the shared
 * policy can name.
 */

#include "util/host_allocator.h"
#include "util/host_allocator_layout.h"

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char *what) {
    std::printf("    [%s] %s\n", ok ? "OK  " : "FAIL", what);
    if (!ok) ++failures;
}

/// Writes a recognisable pattern and checks it is still there.
bool write_and_verify(void *p, size_t n, unsigned char seed) {
    unsigned char *b = static_cast<unsigned char *>(p);
    for (size_t i = 0; i < n; ++i)
        b[i] = static_cast<unsigned char>(seed + (i & 0x7F));
    for (size_t i = 0; i < n; ++i)
        if (b[i] != static_cast<unsigned char>(seed + (i & 0x7F))) return false;
    return true;
}

/// The single-owner store used by its row.  Process-wide so the row can be a
/// pair of plain functions, like the other two already are.
util::SingleOwnerAllocator g_owned;

/**
 * @brief One way in.  The whole point is that the battery cannot tell which.
 */
struct Variant {
    const char *name;
    void *(*alloc)(size_t);
    void (*release)(void *);
};

const Variant kVariants[] = {
    {"general", [](size_t n) { return util::host_alloc(n); },
     [](void *p) { util::host_free(p); }},
    {"per-thread", [](size_t n) { return util::PerThreadAllocator::alloc(n); },
     [](void *p) { util::PerThreadAllocator::free(p); }},
    {"single-owner", [](size_t n) { return g_owned.alloc(n); },
     [](void *p) { g_owned.free(p); }},
};

/**
 * @brief A meeting point: nobody goes on until everybody has arrived.
 *
 * IT IS NOT DECORATION, and getting this wrong made a check pass on one system
 * and fail on the other for a reason that had nothing to do with the code.
 * Starting the threads together only guarantees they EXIST at the same time.
 * What the owner-id pool bounds is how many hold a cache AT ONCE, and a body
 * that allocates and frees in a microsecond never gets there: on Linux the
 * first threads were done and had already handed their ids back before the last
 * ones were scheduled, so 128 threads never needed more than a handful of ids.
 * The test was measuring the scheduler.
 */
struct Barrier {
    std::atomic<int> arrived{0};
    const int total;
    explicit Barrier(int n) noexcept : total(n) {}
    void wait() noexcept {
        arrived.fetch_add(1, std::memory_order_acq_rel);
        while (arrived.load(std::memory_order_acquire) < total) {
        }
    }
};

/// Runs @p body on @p n threads at once.  The body is handed the barrier so it
/// can hold whatever it took until every other thread has taken one too.
template <typename F> void in_parallel(int n, F body) {
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    Barrier all(n);
    std::vector<std::thread> ts;
    ts.reserve(size_t(n));
    for (int t = 0; t < n; ++t) {
        ts.emplace_back([&, t] {
            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) {
            }
            body(t, all);
        });
    }
    while (ready.load() < n) {
    }
    go.store(true, std::memory_order_release);
    for (auto &th : ts) th.join();
}

/// The part that has to hold for all of them.
void common_battery(const Variant &v) {
    std::printf("  -- %s\n", v.name);

    // 1.  Serves, aligned, writable, and no two live blocks overlap.
    {
        const size_t sizes[] = {16, 48, 64, 200, 512, 2048};
        const int n = int(sizeof(sizes) / sizeof(sizes[0]));
        void *p[6];
        bool aligned = true, writable = true;
        for (int i = 0; i < n; ++i) {
            p[i] = v.alloc(sizes[i]);
            if (p[i] == nullptr) {
                writable = false;
                break;
            }
            if ((reinterpret_cast<uintptr_t>(p[i]) & (util::kAlign - 1)) != 0)
                aligned = false;
            if (!write_and_verify(p[i], sizes[i], (unsigned char)(i * 31)))
                writable = false;
        }
        check(aligned, "every block comes aligned");
        check(writable, "and can be written end to end");

        /* Overlap is checked by re-reading AFTER writing them all: if two of
         * them shared storage, the later write would have eaten the earlier
         * pattern. */
        bool intact = true;
        for (int i = 0; i < n && intact; ++i) {
            const unsigned char *b = static_cast<unsigned char *>(p[i]);
            for (size_t j = 0; j < sizes[i]; ++j)
                if (b[j] != (unsigned char)(i * 31 + (j & 0x7F))) {
                    intact = false;
                    break;
                }
        }
        check(intact, "two live blocks never overlap");
        for (int i = 0; i < n; ++i) v.release(p[i]);
    }

    // 2.  Interchangeable with the general allocator, both ways.
    {
        void *a = v.alloc(64);
        util::host_free(a); // the general one takes ours
        void *b = util::host_alloc(64);
        v.release(b); // and we take the general one's
        void *c = v.alloc(64);
        void *d = util::host_alloc(64);
        check(c != nullptr && d != nullptr && c != d,
              "blocks cross between the two, and both keep serving after");
        v.release(c);
        util::host_free(d);
    }

    // 3.  Frees are LOCAL in both regions.
    {
        const size_t sizes[] = {64, util::kBigClassMin * 2};
        for (size_t n : sizes) {
            const util::HostAllocStats a = util::host_alloc_stats();
            const int rounds = 4000;
            std::vector<void *> blocks;
            blocks.reserve(size_t(rounds));
            for (int i = 0; i < rounds; ++i) blocks.push_back(v.alloc(n));
            for (void *p : blocks) v.release(p);
            const util::HostAllocStats b = util::host_alloc_stats();
            const uint64_t remote = b.remote_frees - a.remote_frees;
            check(remote == 0, n < util::kBigClassMin
                                   ? "small blocks are freed locally"
                                   : "BIG-region blocks too: no atomic per free");
        }
    }

    // 4.  What another thread releases comes back and is reused.
    {
        const int n = 400;
        std::vector<void *> blocks(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) blocks[size_t(i)] = v.alloc(64);
        std::thread t([&] {
            for (int i = 0; i < n; ++i) util::host_free(blocks[size_t(i)]);
        });
        t.join();

        /* If those blocks had been lost rather than returned through the
         * owner's atomic stack, this loop runs the region down instead of
         * recycling them. */
        bool alive = true;
        for (int i = 0; i < n; ++i) {
            void *p = v.alloc(64);
            if (p == nullptr) {
                alive = false;
                break;
            }
            v.release(p);
        }
        check(alive, "what ANOTHER thread released is recovered and reused");
    }
}

/// What only the single-owner one can be asked.
void single_owner_only() {
    std::printf("  -- single-owner, on its own\n");
    check(g_owned.valid(), "it gets a store of its own");

    const uint64_t before = g_owned.stats().small_allocs;
    for (int i = 0; i < 100; ++i) g_owned.free(g_owned.alloc(32));
    check(g_owned.stats().small_allocs - before >= 100,
          "its allocations are counted in ITS store");

    util::SingleOwnerAllocator other;
    /* `stats()` is by value: it fills the total in when asked, because the hot
     * path only keeps the per-tag split. */
    check(!other.valid() || other.stats().small_allocs == 0,
          "a fresh store starts at zero");
    void *p1 = g_owned.alloc(48);
    void *p2 = other.alloc(48);
    check(p1 != p2, "two stores never hand out the same block");
    g_owned.free(p1);
    other.free(p2);
}

/// What only the per-thread one can be asked.
void per_thread_only() {
    std::printf("  -- per-thread, on its own\n");
    const int n = int(util::kMaxThreads) * 2; // well past the shared bound

    /* THE CONTROL FIRST, and it is the half that makes the other half worth
     * anything.  The same threads on the general allocator have to run out of
     * owner ids: if they did not, this workload would not be reaching the case
     * at all, and the check below would pass while measuring nothing.  A green
     * test that proves nothing is worse than no test, because it is trusted. */
    const uint64_t shared_before = util::host_alloc_stats().no_owner_id;
    in_parallel(n, [](int, Barrier &all) {
        void *p = util::host_alloc(128); // this is what takes an owner id
        all.wait();                      // and it is HELD until everyone has one
        util::host_free(p);
    });
    const uint64_t shared_after = util::host_alloc_stats().no_owner_id;
    std::printf("      %d threads at once, kMaxThreads=%u, ran out %llu times\n",
                n, (unsigned)util::kMaxThreads,
                (unsigned long long)(shared_after - shared_before));
    check(shared_after > shared_before,
          "control: on the general allocator these threads DO run out of owner "
          "ids, so the case is really being reached");

    const uint64_t before = util::host_per_thread_exhausted();
    std::atomic<int> served{0};
    in_parallel(n, [&](int, Barrier &all) {
        void *p = util::PerThreadAllocator::alloc(128);
        if (p != nullptr && write_and_verify(p, 128, 0x11)) served.fetch_add(1);
        all.wait(); // hold the cache until all n threads have one of their own
        util::PerThreadAllocator::free(p);
    });
    check(served.load() == n, "every thread got served");
    check(util::host_per_thread_exhausted() == before,
          "with twice kMaxThreads alive, NO thread fell back to the lock");

    /* Ids have to come back when a thread dies.  Far more threads than there
     * are ids, a few at a time: without recycling this drains the pool and
     * every later thread ends up behind the lock, for good. */
    for (int round = 0; round < 8; ++round)
        in_parallel(32, [](int, Barrier &all) {
            void *p = util::PerThreadAllocator::alloc(64);
            all.wait(); // all 32 hold a cache at once, then all give it back
            util::PerThreadAllocator::free(p);
        });
    check(util::host_per_thread_exhausted() == before,
          "256 threads in a row and the pool never ran out: ids come back");
}

} // namespace

int main() {
    std::printf("== the allocator variants, one battery ==\n");
    if (!util::host_alloc_active()) {
        std::printf("  allocator off (VESTA_NO_HOST_SLAB): nothing to check\n");
        return 0;
    }
    std::printf("  kMaxThreads=%u  kPerThreadCaches=%u  kBigClassMin=%zu\n\n",
                (unsigned)util::kMaxThreads, (unsigned)util::kPerThreadCaches,
                util::kBigClassMin);

    for (const Variant &v : kVariants) common_battery(v);
    single_owner_only();
    per_thread_only();

    std::printf(failures == 0 ? "ALL OK\n" : "%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}

/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file bench/bench_vs_malloc.cpp
 * @brief Head to head against malloc/calloc/realloc, in one process.
 *
 * WHY A SECOND BENCHMARK.  It used to be that `bench_allocator` compared by
 * re-running the same binary with the allocator switched off, and the two
 * numbers came from different runs -- so anything that drifted between them
 * (CPU frequency, page cache, another process waking up) landed on the
 * difference.  That switch is gone and `bench_allocator` now interleaves both
 * columns in one process as well, through `support/system_alloc.h`.
 *
 * What still separates the two: there the patterns are whole-program shapes
 * (bursts, threads, cross-thread), here each call is measured on its own,
 * including `realloc`, which the other one does not exercise at all.
 *
 * Here both allocators are called directly, in the same process, back to back.
 *
 * INTERLEAVED, AND IN BOTH ORDERS.  Every measurement runs A-B-B-A and averages
 * each side.  Two runs in a row do not distinguish a change from drift: if the
 * machine speeds up or slows down during the benchmark, a plain A-then-B blames
 * the allocator for it.  A-B-B-A cancels a linear drift.
 *
 * WHAT IS MEASURED, and why each one is here:
 *
 *   1. hot     allocate and free immediately.  Best case: the block comes
 *              straight back off the free list, still in cache.
 *   2. burst   allocate a batch, then free the batch.  A compiler's usual
 *              shape, and where size classes earn their keep.
 *   3. churn   a live set with random insert/remove.  The realistic one, and
 *              the one that punishes fragmentation -- the other two never make
 *              an allocator work.
 *   4. calloc  the same, zeroed, and measured TWICE: once with the caller
 *              reading the whole block and once reading a sixty-fourth.  It has
 *              to be both, because a large `calloc` does not zero anything --
 *              it maps pages the kernel already holds zeroed and defers the
 *              cost to page faults.  Measured with a single byte touched, that
 *              deferral looks like speed; measured with the block actually
 *              used, it is 8x slower.  Neither row alone is the answer.
 *   5. realloc growth from small to large, which is where a bad realloc shows
 *              up as a copy it did not need to make.
 *
 * Sizes go from 16 bytes to 1 MiB, because the answer changes completely along
 * the way: small allocations are dominated by the free-list path and large ones
 * by whatever the allocator does with the operating system.
 */

#include "util/alloc/host_allocator.h"
#include "util/alloc/host_allocator_c.h"
#include "util/os/os_memory.h"
#include "util/alloc/host_allocator_layout.h"
#include "util/mem/vesta_memcpy.h"
#include "util/mem/vesta_memset.h"

#include "affinity.h"
#include "bench_stats.h"
#include "report.h"
#include "system_alloc.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double ns_since(Clock::time_point t0, long long ops) {
    const auto dt = Clock::now() - t0;
    return double(std::chrono::duration_cast<std::chrono::nanoseconds>(dt)
                      .count()) /
           double(ops);
}

/// A tiny deterministic generator.  `rand()` takes a lock in some libcs, which
/// would end up in the measurement.
struct Rng {
    uint64_t s = 0x9E3779B97F4A7C15ull;
    uint32_t next() noexcept {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return uint32_t(s >> 32);
    }
};

// ---------------------------------------------------------------------------
//  Every way in, behind the same shape so the loops are identical.
//
//  A column is a TYPE, which is also how a caller picks a specialisation in
//  real code -- resolved at compile time, never a flag.  Each one carries its
//  own name and says whether it is one of ours, so the driver below can walk
//  them as a list instead of being hand-unrolled once per column.
// ---------------------------------------------------------------------------

struct Ours {
    static void *alloc(size_t n) noexcept { return util::host_alloc(n); }
    static void *zeroed(size_t n) noexcept { return vesta_host_calloc(1, n); }
    static void *grow(void *p, size_t n) noexcept {
        return vesta_host_realloc(p, n);
    }
    static void release(void *p) noexcept { util::host_free(p); }
    static const char *name() noexcept { return "shared"; }
    static constexpr bool is_ours = true;
};

/// A cache per thread and no lock.  Here it measures the SAME single-threaded
/// work as the others on purpose: this is where it must not be paying for the
/// thing it buys.  What it buys only shows up in `bench_contention`, with more
/// threads alive than the shared policy can name.
struct PerThread {
    static void *alloc(size_t n) noexcept {
        return util::PerThreadAllocator::alloc(n);
    }
    static void *zeroed(size_t n) noexcept {
        void *p = util::PerThreadAllocator::alloc(n);
        if (p != nullptr) vesta_memset(p, 0, n);
        return p;
    }
    static void *grow(void *p, size_t n) noexcept {
        return util::host_realloc(p, n);
    }
    static void release(void *p) noexcept { util::PerThreadAllocator::free(p); }
    static const char *name() noexcept { return "per-thread"; }
    static constexpr bool is_ours = true;
};

/// El de un solo dueno: la misma maquinaria sin pasar por la ranura del hilo.
/// Es un objeto, no un modo global, asi que convive con los otros dos.
util::SingleOwnerAllocator g_owned;
struct Owned {
    static void *alloc(size_t n) noexcept { return g_owned.alloc(n); }
    static void *zeroed(size_t n) noexcept {
        void *p = g_owned.alloc(n);
        if (p != nullptr) vesta_memset(p, 0, n);
        return p;
    }
    static void *grow(void *p, size_t n) noexcept {
        return util::host_realloc(p, n);
    }
    static void release(void *p) noexcept { g_owned.free(p); }
    static const char *name() noexcept { return "owned"; }
    static constexpr bool is_ours = true;
};

/* THE SYSTEM'S, AND IT USED TO BE `std::malloc`, WHICH WAS THIS BENCHMARK
 * COMPARING ITSELF WITH ITSELF.
 *
 * Since the interposition landed, `malloc` in this process IS this allocator:
 * the linker renamed the call.  So the column labelled `malloc` was our own
 * fast path with a thunk in front of it, and the "1.47x better than the system"
 * it printed was the price of that thunk, not a win over anybody.  Measured
 * side by side: this column read 1.98 ns while the real msvcrt, reached the way
 * below, reads 11.9.  Six times off, and nothing in the output said so.
 *
 * `support/system_alloc.h` reaches the genuine one -- a private copy of the C
 * runtime on Windows, libc by handle on ELF -- and the four entry points travel
 * together because a block must go back to the pair that made it.
 *
 * Cached in plain globals rather than asked for per call: `api()` is a
 * function-local static, and its guard check inside the timed loop would be
 * measured as if it were part of the allocator. */
void *(*g_sys_alloc)(size_t) = nullptr;
void *(*g_sys_zeroed)(size_t, size_t) = nullptr;
void *(*g_sys_grow)(void *, size_t) = nullptr;
void (*g_sys_free)(void *) = nullptr;

struct System {
    static void *alloc(size_t n) noexcept { return g_sys_alloc(n); }
    static void *zeroed(size_t n) noexcept { return g_sys_zeroed(1, n); }
    static void *grow(void *p, size_t n) noexcept { return g_sys_grow(p, n); }
    static void release(void *p) noexcept { g_sys_free(p); }
    static const char *name() noexcept { return "system"; }
    static constexpr bool is_ours = false;
};

/**
 * @brief The columns, as a type list.
 *
 * Adding a way in is a TYPE here.  It used to be a field in the result struct,
 * a pair of lines in the A-B-B-A driver and a column in two printf formats,
 * which is three places to forget and no error when you do -- the column simply
 * would not appear.
 *
 * Order matters: the driver walks this forwards and then backwards, so the
 * system allocator goes last and lands first on the way back.
 */
template <class...> struct Columns {};
using AllColumns = Columns<Ours, PerThread, Owned, System>;
constexpr int kCols = 4;

// ---------------------------------------------------------------------------
//  The patterns.  Each returns nanoseconds per operation.
// ---------------------------------------------------------------------------

template <class A> double hot(size_t size, int rounds) {
    const auto t0 = Clock::now();
    for (int i = 0; i < rounds; ++i) {
        void *p = A::alloc(size);
        // Touch the first byte: an allocator that hands back an address it
        // never committed would otherwise not be charged for it.
        if (p != nullptr) *static_cast<volatile char *>(p) = 1;
        A::release(p);
    }
    return ns_since(t0, 2LL * rounds);
}

template <class A> double burst(size_t size, int batch, int rounds) {
    std::vector<void *> live(static_cast<size_t>(batch));
    const auto t0 = Clock::now();
    for (int r = 0; r < rounds; ++r) {
        for (int i = 0; i < batch; ++i) {
            live[size_t(i)] = A::alloc(size);
            if (live[size_t(i)] != nullptr)
                *static_cast<volatile char *>(live[size_t(i)]) = 1;
        }
        for (int i = 0; i < batch; ++i)
            A::release(live[size_t(i)]);
    }
    return ns_since(t0, 2LL * batch * rounds);
}

/// A live set of @p live blocks; each step frees a random one and allocates a
/// replacement.  This is the one that makes an allocator work.
template <class A> double churn(size_t size, int live_n, int steps) {
    std::vector<void *> live(static_cast<size_t>(live_n));
    for (int i = 0; i < live_n; ++i)
        live[size_t(i)] = A::alloc(size);

    Rng rng;
    const auto t0 = Clock::now();
    for (int i = 0; i < steps; ++i) {
        const size_t slot = rng.next() % uint32_t(live_n);
        A::release(live[slot]);
        live[slot] = A::alloc(size);
        if (live[slot] != nullptr)
            *static_cast<volatile char *>(live[slot]) = 1;
    }
    const double r = ns_since(t0, 2LL * steps);

    for (int i = 0; i < live_n; ++i)
        A::release(live[size_t(i)]);
    return r;
}

/**
 * @brief Stops the compiler from deleting the work being measured.
 *
 * NEEDED, and this benchmark got it wrong without it.  With `std::free` right
 * after the block, GCC knows it dies and DELETES the memset; with our `free`,
 * an opaque call, it cannot.  So the two columns were not measuring the same
 * thing: 4 MiB "filled" in 9 us is 447 GB/s, which no single core can do.
 */
[[gnu::always_inline]] inline void keep(void *p) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("" : : "r"(p) : "memory");
#else
    (void)*static_cast<volatile char *>(p);
#endif
}

/**
 * @brief Zeroed allocation, measured by how much of it the caller USES.
 *
 * WHY THIS IS TWO CASES AND NOT ONE.  A large `calloc` does not zero anything:
 * it maps pages the kernel already holds zeroed and defers the cost to page
 * faults.  If nobody touches the memory, that cost is never paid.  We zero for
 * real, up front.  Measuring with a single byte touched therefore does not
 * compare two ways of doing the same job -- it rewards whoever does less work,
 * without asking whether the work was needed.
 *
 * Measured here, 1 MiB on Windows: zeroing eagerly and then reading the whole
 * block costs 28,7 us; deferring costs 4,7 us plus 292 us of page faults.
 * Deferring only wins when the caller touches under ~4% of what it asked for.
 *
 * @param used  Fraction of the block the caller reads, 1..N (1 = all of it).
 */
template <class A> double zeroed(size_t size, int rounds, size_t used) {
    const auto t0 = Clock::now();
    for (int i = 0; i < rounds; ++i) {
        void *p = A::zeroed(size);
        if (p != nullptr) {
            std::memset(p, 1, size / used);
            keep(p);
        }
        A::release(p);
    }
    return ns_since(t0, 2LL * rounds);
}

/// Grow 64 bytes up to @p size, doubling.  Counts one op per step.
template <class A> double grow(size_t size, int rounds) {
    long long ops = 0;
    const auto t0 = Clock::now();
    for (int r = 0; r < rounds; ++r) {
        void *p = A::alloc(64);
        for (size_t n = 128; n <= size; n *= 2) {
            void *q = A::grow(p, n);
            if (q == nullptr) break;
            p = q;
            ++ops;
        }
        A::release(p);
    }
    return ops == 0 ? 0.0 : ns_since(t0, ops);
}

// ---------------------------------------------------------------------------
//  Measuring, A-B-B-A.
// ---------------------------------------------------------------------------

/**
 * @brief How many times each row is measured.
 *
 * IT USED TO BE TWO -- one forwards, one backwards, averaged -- and two samples
 * cannot tell a difference from a hiccup.  The sister benchmarks settled this
 * long ago: repeats, summarised by the clean half, with the spread kept so a
 * row that moved is not allowed to name a winner.  See `support/bench_stats.h`.
 *
 * Odd on purpose: the summaries index a median, and with an even count that is
 * a coin flip between two samples.
 */
constexpr int kReps = 11;

/**
 * @brief What this machine cannot resolve, measured rather than assumed.
 *
 * Filled in by @c calibrate, which runs THIS allocator in every column so that
 * the true ratio is 1.00x by construction: whatever comes out instead is the
 * size of the lie the machine tells today, and every row has to beat it before
 * it is allowed to name a winner.
 */
double g_floor = 0.02;

/// Which pass of the machine is being measured, on a hybrid part.
const char *g_pass = "";

struct Row {
    double ns[kCols] = {};
    /* And what each one KEEPS.  Time alone does not settle any of these rows:
     * an allocator that defers the work also defers the memory, and one that
     * holds on to a block is faster next time for a reason that has a price.
     * These are the committed bytes this process gained across the run. */
    double mem[kCols] = {};
    /// System over the best of ours, paired repeat by repeat -- NOT the
    /// quotient of the two summaries.  See @c measure.
    double ratio = 0.0;
    /// How much that ratio moved between repeats, which is what decides whether
    /// the row may declare anything.  The columns' own spread is deliberately
    /// not kept: see @c row for why the two cannot be shown together.
    double ratio_spread = 0.0;
};

/**
 * @brief How much memory each column had to COMMIT for its run.
 *
 * The two allocators live in the same process, so a process-level number
 * cannot tell them apart.  Each is therefore read with the instrument that
 * attributes it:
 *
 *   - ours (both columns) by our own counter of bytes committed out of the
 *     region, which is exact and counts only us;
 *   - malloc by what the process gained in committed private bytes while only
 *     its column was running.
 *
 * Reading ours with the process counter instead would print zeros and mean
 * nothing: by the time a case runs, the region already holds what the earlier
 * cases committed.
 */
uint64_t committed_by_us() noexcept {
    return util::host_alloc_stats().bytes_reserved;
}
uint64_t committed_by_process() noexcept {
    return util::os_process_memory().commit;
}

/* Los casos son structs de ambito de fichero y no lambdas locales porque hacen
 * falta con un `operator()` de PLANTILLA -- el mismo bucle se instancia para
 * los dos asignadores --, y C++17 no permite plantillas miembro dentro de una
 * clase local.  Una lambda generica tampoco vale: el parametro es el TIPO del
 * asignador, no un valor. */
struct HotCase {
    size_t size;
    int rounds;
    template <class A> double operator()() const {
        return hot<A>(size, rounds);
    }
};
struct BurstCase {
    size_t size;
    int rounds;
    template <class A> double operator()() const {
        return burst<A>(size, 512, rounds);
    }
};
struct ChurnCase {
    size_t size;
    int rounds;
    template <class A> double operator()() const {
        return churn<A>(size, 1024, rounds);
    }
};
struct ZeroedCase {
    size_t size;
    int rounds;
    size_t used; ///< 1 = the caller reads all of it; 64 = a sixty-fourth
    template <class A> double operator()() const {
        return zeroed<A>(size, rounds, used);
    }
};
struct GrowCase {
    size_t size;
    int rounds;
    template <class A> double operator()() const {
        return grow<A>(size, rounds);
    }
};

/// One column's run, behind a pointer so the driver can hold them in an array.
/// The call happens once per RUN -- thousands of operations later -- so the
/// inner loop is still `A`'s code, fully typed and fully inlined.
template <class F, class A> double call_one(const F &run) {
    return run.template operator()<A>();
}

/// Whether a column is served by this library, which decides WHICH instrument
/// attributes its memory.
template <class A> bool column_is_ours() { return A::is_ours; }
template <class A> const char *column_name() { return A::name(); }

/**
 * @brief Measures every column @c kReps times and summarises them.
 *
 * FORWARDS AND THEN BACKWARDS, alternating by repeat, which is A-B-B-A
 * generalised: each column lands as often near the start as near the end, so
 * drift in the machine -- thermal, another process, the scheduler settling --
 * does not land on the difference between them.
 *
 * THE RATIO IS PAIRED INSIDE THE LOOP, not divided afterwards.  If the machine
 * slows down during one repeat, every column of that repeat slows down together
 * and their quotient does not notice; the quotient of two summaries does,
 * because each column absorbs the slowdown differently.  It is the difference
 * between comparing two things and comparing two measurements of two things,
 * and it shows up exactly on the rows that sit near the margin.
 *
 * @param run  Called as `run.template operator()<Alloc>()`, returns ns/op.
 */
template <class F, class... A> Row measure(const F &run, Columns<A...>) {
    using Fn = double (*)(const F &);
    static const Fn fns[] = {&call_one<F, A>...};
    static const bool ours[] = {A::is_ours...};
    constexpr int n = int(sizeof...(A));

    Row r;
    double s[kCols][kReps];
    double ratios[kReps];

    for (int rep = 0; rep < kReps; ++rep) {
        for (int k = 0; k < n; ++k) {
            const int i = (rep & 1) ? n - 1 - k : k;
            /* The memory each one keeps is read around its FIRST run only: by
             * the second the heap has already grown and the difference would
             * read as zero for everybody.  Ours is read with our own
             * committed-bytes counter, which is exact and counts only us; the
             * system one with what the process gained, which is the only
             * instrument that sees it. */
            if (rep == 0) {
                const uint64_t before =
                    ours[i] ? committed_by_us() : committed_by_process();
                s[i][rep] = fns[i](run);
                const uint64_t after =
                    ours[i] ? committed_by_us() : committed_by_process();
                /* Signed on purpose: a negative reading means that column gave
                 * memory back to the system, which is a real answer and not an
                 * error to hide. */
                r.mem[i] = double(int64_t(after) - int64_t(before));
            } else {
                s[i][rep] = fns[i](run);
            }
        }
    }

    /* SUMMARISE FIRST, then pick which column the ratio is against.  `summarize`
     * sorts in place, so the samples are copied for the pairing below. */
    double kept[kCols][kReps];
    for (int i = 0; i < n; ++i)
        for (int rep = 0; rep < kReps; ++rep) kept[i][rep] = s[i][rep];

    for (int i = 0; i < n; ++i) {
        double noise = 0.0; // measured by `summarize`, not reported; see `Row`
        r.ns[i] = bench_stats::summarize(s[i], kReps, &noise);
    }

    /* AGAINST THE BEST OF OURS, chosen ONCE from the summaries rather than
     * repeat by repeat.  The question is "what can be had from this library",
     * not "which of its forms did we pick" -- but taking the minimum inside the
     * loop answered a third question nobody asked: which of the three got lucky
     * this time.  With the three columns within a quarter of each other, that
     * minimum is a coin toss, and it dragged the ratio with it: 113% of spread
     * on a row whose columns each moved a few per cent.  Picking the column
     * first and pairing against THAT one keeps the pairing -- both numbers from
     * the same repeat, so a slow patch of machine cancels -- without the jitter
     * of a statistic that changes which sample it names. */
    int best = 0;
    for (int i = 1; i < n - 1; ++i)
        if (r.ns[i] > 0.0 && (r.ns[best] <= 0.0 || r.ns[i] < r.ns[best]))
            best = i;
    for (int rep = 0; rep < kReps; ++rep)
        ratios[rep] = kept[best][rep] > 0.0
                          ? kept[n - 1][rep] / kept[best][rep]
                          : 0.0;
    r.ratio = bench_stats::summarize_ratio(ratios, kReps, &r.ratio_spread);
    return r;
}

void row(const char *label, size_t size, const Row &r) {
    char name[48];
    if (size >= (1u << 20))
        std::snprintf(name, sizeof(name), "%s %zuM", label, size >> 20);
    else if (size >= 1024)
        std::snprintf(name, sizeof(name), "%s %zuK", label, size >> 10);
    else
        std::snprintf(name, sizeof(name), "%s %zu", label, size);

    std::printf("  %-16s", name);
    for (int i = 0; i < kCols; ++i) std::printf(" %10.2f", r.ns[i]);

    /* The ratio was measured, not divided here; see @c measure.  Every column
     * stays on show so the cost of sharing is visible. */
    std::printf("  %6.2fx", r.ratio);

    /* AND WHETHER IT MAY BE BELIEVED.  A row has to beat the floor this machine
     * was measured at, or its own spread if that is worse, before it is allowed
     * to say who won.  Without this a 3% difference read as a result on a
     * machine that cannot resolve 20%. */
    const double margin =
        r.ratio_spread > g_floor ? r.ratio_spread : g_floor;
    const double d = r.ratio > 1.0 ? r.ratio - 1.0 : 1.0 - r.ratio;
    const char *verdict;
    const char *tint;
    if (r.ratio <= 0.0) {
        verdict = "-";
        tint = report::dim();
    } else if (d <= margin) {
        verdict = "too close";
        tint = report::amber();
    } else if (r.ratio > 1.0) {
        verdict = "ours";
        tint = report::green();
    } else {
        verdict = "SYSTEM";
        tint = report::red();
    }
    /* THE RATIO'S SPREAD AND NOT THE TIMES', and it is the one that answers the
     * question the row is asking: whether the COMPARISON holds.  The columns'
     * own spread was tried here as a second figure and taken out again --
     * `summarize` measures how far the median sits from the minimum while
     * `summarize_ratio` measures the gap between quartiles, so the two numbers
     * are not on the same scale and printing them side by side invites exactly
     * the comparison that cannot be made. */
    std::printf(" %s%-9s%s %s%4.0f%%%s  ", tint, verdict, report::reset(),
                report::dim(), r.ratio_spread * 100.0, report::reset());

    const double mib = 1024.0 * 1024.0;
    for (int i = 0; i < kCols; ++i) std::printf(" %7.2f", r.mem[i] / mib);
    std::printf("\n");
}

void header(const char *title) {
    static const char *names[] = {Ours::name(), PerThread::name(),
                                  Owned::name(), System::name()};
    std::printf("\n%s\n", title);
    std::printf("  %-16s", "case");
    for (int i = 0; i < kCols; ++i) std::printf(" %10s", names[i]);
    std::printf("  %7s %-9s %5s   %s\n", "sys/best", "verdict", "spread",
                "MiB committed by the run");
    std::printf("  %-16s", "");
    for (int i = 0; i < kCols; ++i) std::printf(" %10s", "ns/op");
    std::printf("  %7s %-9s %5s  ", "", "", "");
    for (int i = 0; i < kCols; ++i) std::printf(" %7s", names[i]);
    std::printf("\n  ----------------");
    for (int i = 0; i < kCols; ++i) std::printf(" ----------");
    std::printf("  ------- --------- -----  ");
    for (int i = 0; i < kCols; ++i) std::printf(" -------");
    std::printf("\n");
}

const size_t kSizes[] = {16, 64, 256, 1024, 4096, 65536, 1u << 20};
const int kSizeCount = int(sizeof(kSizes) / sizeof(kSizes[0]));

/**
 * @brief Sizes only the zeroed cases use, and why they are only there.
 *
 * Above a few mebibytes the interesting question stops being the free list and
 * becomes WHO puts the zeros there -- and the answer flips: past a threshold it
 * is cheaper to hand the pages back to the system and let it zero what the
 * caller actually touches.  Without a size up here, that path would not be
 * exercised by anything, and a path nothing measures is a path nobody knows is
 * working.
 *
 * They are not in `kSizes` because the other cases keep a live set of hundreds
 * of blocks: at eight mebibytes each that is gigabytes of working set, which
 * would measure the machine's memory and not the allocator.
 */
const size_t kBigZeroedSizes[] = {size_t(8) << 20};
const int kBigZeroedCount =
    int(sizeof(kBigZeroedSizes) / sizeof(kBigZeroedSizes[0]));

// ---------------------------------------------------------------------------
//  Lo que el reloj no ve: lo que cuesta un bloque en MEMORIA
// ---------------------------------------------------------------------------
//
// Las tablas de arriba miden tiempo, y hay cambios que no lo tocan y sin
// embargo empeoran el asignador: mover una clase de tamano, cambiar la cabecera
// del trozo o el relleno de las reservas alineadas cambia cuanta memoria se
// queda quieta sin que ninguna cifra de nanosegundos se mueva.
//
// Estas dos tablas son ARITMETICA, no medida: salen iguales en cualquier
// maquina y en cualquier corrida.  Eso las hace mejores detectoras de
// regresion que un tiempo -- una diferencia aqui es un cambio, nunca ruido --.

/// La clase que serviria @p n bytes, buscada recorriendo la lista.  La tabla
/// rapida se llena al arrancar el asignador; aqui hace falta la aritmetica pura.
size_t class_for(size_t n) {
    for (uint32_t i = 0; i < util::kClasses; ++i)
        if (util::kSizes[i] >= n) return util::kSizes[i];
    return 0; // por encima de las clases: lo sirve un tramo
}

/// Cuanto se queda sin usar al final de un trozo, por clase.  Un trozo no se
/// devuelve hasta vaciarse entero, asi que lo que sobra al final esta retenido
/// mientras viva UN solo bloque de esa clase.
void memory_per_class() {
    std::printf("\nwhat a chunk wastes, per size class\n");
    std::printf("  %8s %7s %8s %10s %8s\n", "class", "chunk", "blocks",
                "left over", "of chunk");
    std::printf("  -------- ------- -------- ---------- --------\n");
    for (uint32_t i = 0; i < util::kClasses; ++i) {
        const size_t s = util::kSizes[i];
        /* Cada clase se sirve del trozo que le toca, y hay que preguntarlo aqui
         * en vez de dar por hecho el pequeno: las clases grandes viven en su
         * propia region con trozos de otro tamano, que es justamente el cambio
         * que esta tabla tiene que poder detectar. */
        const size_t chunk =
            s >= util::kBigClassMin ? util::kBigChunkBytes : util::kChunkBytes;
        const size_t usable = chunk - sizeof(util::ChunkHeader);
        const size_t blocks = usable / s;
        const size_t left = usable - blocks * s;
        const double pct = 100.0 * double(left) / double(chunk);
        // Solo lo que se nota: por debajo del 1% la lista entera seria ruido.
        if (pct >= 1.0)
            std::printf("  %8zu %6zuK %8zu %10zu %7.1f%%\n", s, chunk >> 10,
                        blocks, left, pct);
    }
}

/// Lo que cuesta de mas un tipo SOBRE-ALINEADO frente a uno normal del mismo
/// tamano.  Es el precio del relleno, y se compara contra la clase ideal para
/// que un cambio en la formula salte aqui aunque no mueva ningun tiempo.
void memory_over_aligned() {
    struct Caso {
        size_t n, align;
    };
    static const Caso kCasos[] = {{32, 32},     {64, 64},   {128, 64},
                                  {1000, 64},   {4096, 64}, {1024, 256},
                                  {4096, 4096}};

    std::printf("\nwhat over-alignment costs (padding, not time)\n");
    std::printf("  %8s %7s %9s %9s %8s\n", "size", "align", "ideal", "actual",
                "x ideal");
    std::printf("  -------- ------- --------- --------- --------\n");
    for (const Caso &c : kCasos) {
        void *p = util::host_alloc_aligned(c.n, c.align);
        if (p == nullptr) continue;
        /* La clase real se lee de la CABECERA del trozo del bloque original,
         * no se recalcula: repetir aqui la formula del relleno mediria la
         * formula y no el codigo. */
        void *raw = nullptr;
        vesta_memcpy(&raw, reinterpret_cast<const char *>(p) - sizeof(void *),
                     sizeof raw);
        const util::ChunkHeader *h = util::chunk_of(raw);
        const size_t actual = (h->magic == util::kChunkMagic)
                                  ? util::kSizes[h->cls]
                                  : 0; // lo sirvio un tramo
        const size_t ideal = class_for(c.n);
        std::printf("  %8zu %7zu %9zu %9zu %7.2fx\n", c.n, c.align, ideal,
                    actual, (ideal && actual) ? double(actual) / double(ideal)
                                              : 0.0);
        util::host_free_aligned(p);
    }
}

/// Fewer iterations as the blocks get bigger, or the large cases dominate the
/// wall time without adding information.
int rounds_for(size_t size) {
    if (size <= 1024) return 300000;
    if (size <= 65536) return 40000;
    return 4000;
}

/**
 * @brief The control: this allocator in EVERY column, where the answer is 1.00x.
 *
 * Same driver, same repeats, same summarising -- the only difference is that
 * both sides of the comparison run the same code, so anything the ratio reports
 * other than 1.00x came from the machine and not from the allocators.
 */
using ControlColumns = Columns<Ours, Ours, Ours, Ours>;

/**
 * @brief Measures the verdict margin for this pass.
 *
 * Several sizes, because the floor is not the same at all of them: on small
 * blocks a round lasts a few nanoseconds and the clock weighs more.  The WORST
 * is taken, which is the only safe choice -- with the average, the small rows
 * would keep deciding above their means.
 *
 * Measured once per pass: a small core does not resolve what a big one does, so
 * carrying the number over would describe the wrong machine.
 */
void calibrate() {
    g_floor = 0.02;
    double worst = 0.0;
    for (size_t s : {size_t(16), size_t(256), size_t(4096)}) {
        const Row c = measure(HotCase{s, rounds_for(s) / 4}, ControlColumns{});
        if (c.ratio <= 0.0) continue;
        const double d = c.ratio > 1.0 ? c.ratio - 1.0 : 1.0 - c.ratio;
        if (d > worst) worst = d;
    }
    if (worst > g_floor) g_floor = worst;
    std::printf("\n%sVerdict margin floor, measured with this allocator in "
                "every column (so the\ntrue ratio is 1.00x): %s%.1f%%%s.  A row "
                "has to beat that, or its own spread\nif it is worse, before it "
                "names a winner.%s\n",
                report::dim(), report::bold(), g_floor * 100.0, report::dim(),
                report::reset());
}

/// Every table, once per pass.  Extracted so that a hybrid machine can measure
/// them on each kind of core: there is no single "speed of this machine" to
/// report on a part with two.
void run_sections() {
    header("allocate and free immediately (hot)");
    for (int i = 0; i < kSizeCount; ++i) {
        const size_t s = kSizes[i];
        row("hot", s, measure(HotCase{s, rounds_for(s)}, AllColumns{}));
    }

    header("allocate a batch, free the batch (burst)");
    for (int i = 0; i < kSizeCount; ++i) {
        const size_t s = kSizes[i];
        const int n = rounds_for(s) / 512 + 1;
        row("burst", s, measure(BurstCase{s, n}, AllColumns{}));
    }

    header("live set with random replacement (churn)");
    for (int i = 0; i < kSizeCount; ++i) {
        const size_t s = kSizes[i];
        /* EIGHT TIMES the steps of the other patterns, and it is not a whim.
         * A step here is one free and one allocate, so at the counts the rest
         * of this file uses a whole sample lasted under half a millisecond and
         * the row moved 107% between repeats -- it could not hold still long
         * enough to mean anything.  The live set is what makes this pattern
         * different, and it is set up OUTSIDE the timed region, so the extra
         * steps buy resolution without buying setup. */
        row("churn", s,
            measure(ChurnCase{s, rounds_for(s) * 8}, AllColumns{}));
    }

    header("zeroed (calloc), and the caller reads ALL of it");
    for (int i = 0; i < kSizeCount; ++i) {
        const size_t s = kSizes[i];
        row("calloc", s, measure(ZeroedCase{s, rounds_for(s), 1}, AllColumns{}));
    }
    for (int i = 0; i < kBigZeroedCount; ++i) {
        const size_t s = kBigZeroedSizes[i];
        row("calloc", s, measure(ZeroedCase{s, rounds_for(s), 1}, AllColumns{}));
    }

    std::printf("\nThe other half of the answer, not a second opinion: whoever\n"
                "defers the zeroing wins below and loses above.  A row where a\n"
                "1 MiB request is barely read says more about the caller asking\n"
                "for the wrong size than about the allocator.\n");
    header("zeroed (calloc), and the caller reads a SIXTY-FOURTH");
    for (int i = 0; i < kSizeCount; ++i) {
        const size_t s = kSizes[i];
        row("calloc", s,
            measure(ZeroedCase{s, rounds_for(s), 64}, AllColumns{}));
    }
    for (int i = 0; i < kBigZeroedCount; ++i) {
        const size_t s = kBigZeroedSizes[i];
        row("calloc", s,
            measure(ZeroedCase{s, rounds_for(s), 64}, AllColumns{}));
    }

    header("grow from 64 bytes by doubling (realloc)");
    for (int i = 2; i < kSizeCount; ++i) {
        const size_t s = kSizes[i];
        const int n = rounds_for(s) / 8 + 1;
        row("grow to", s, measure(GrowCase{s, n}, AllColumns{}));
    }
}

} // namespace

int main() {
    std::printf("== head to head: this allocator vs malloc ==\n\n");
    if (!util::host_alloc_active()) {
        std::printf("This allocator is NOT in force, so both sides would be\n"
                    "the same malloc.  Check that the static archive really "
                    "linked in.\n");
        return 1;
    }
    /* THE SYSTEM'S, REACHED THE ONLY WAY IT STILL CAN BE.  See `System`: in
     * this process `malloc` is us, so the column has to come from a copy of the
     * C runtime our patch never touched. */
    const system_alloc::Api &sys = system_alloc::api();
    if (!sys.ok()) {
        std::printf("No system column: %s.  There is nothing to compare\n"
                    "against, so this benchmark has no answer to give.\n",
                    sys.why);
        return 1;
    }
    g_sys_alloc = sys.alloc;
    g_sys_zeroed = sys.zeroed;
    g_sys_grow = sys.grow;
    g_sys_free = sys.release;
    std::printf("%ssystem column: %s%s\n", report::dim(), sys.how,
                report::reset());
    std::printf("Both are called directly, in this process, interleaved and\n"
                "repeated, so that drift in the machine does not land on the\n"
                "difference between them.\n");

    // Warm both up: the first allocation registers a thread and asks the OS
    // for memory.  Timing that would measure startup, not steady state.
    for (int i = 0; i < 20000; ++i) {
        util::host_free(util::host_alloc(64));
        g_sys_free(g_sys_alloc(64));
    }

    const std::vector<affinity::Pass> passes = affinity::passes();
    if (affinity::topology().why[0] != 0)
        std::printf("\n%sOnly one pass: %s.%s\n", report::amber(),
                    affinity::topology().why, report::reset());

    for (const affinity::Pass &p : passes) {
        const bool pinned = affinity::pin(p.cpus);
        const char *seen = isa::current_core_kind();
        g_pass = p.name;
        std::printf("\n%s%s#### %s ####%s\n", report::bold(), report::cyan(),
                    p.name, report::reset());
        /* Checked, not assumed: a pin that fails silently would turn the passes
         * into the same measurement repeated, and the report would say there is
         * no difference between kinds of core. */
        if (!p.cpus.empty() && !pinned)
            std::printf("%scould not pin to those cores; this pass measures "
                        "whatever the scheduler gave it%s\n",
                        report::amber(), report::reset());
        else if (seen[0] != '\0')
            std::printf("%srunning on a %s%s\n", report::dim(), seen,
                        report::reset());
        calibrate();
        run_sections();
    }
    affinity::unpin();

    memory_per_class();
    memory_over_aligned();

    /* What the whole run cost in MEMORY, which the ns/op columns cannot show.
     * It belongs next to the times: an allocator that is faster because it
     * keeps everything it was ever given is not faster, it is trading. */
    const util::OsProcessMemory pm = util::os_process_memory();
    const util::HostAllocStats hs = util::host_alloc_stats();
    const double mib = 1024.0 * 1024.0;
    std::printf("\nmemory for the whole run\n");
    std::printf("  address space reserved by us     %8.1f MiB\n",
                double(util::host_region_reserved()) / mib);
    std::printf("  committed by us                  %8.1f MiB   (%llu chunks)\n",
                double(hs.bytes_reserved) / mib,
                (unsigned long long)hs.chunks);
    /* El pico de comprometido no lo da todo el mundo: en Linux vendria de
     * `/proc/self/status` y la capa de sistema no lo lee.  Se dice, en vez de
     * imprimir un cero que parece un dato. */
    if (pm.commit_peak != 0)
        std::printf("  committed by the whole process   %8.1f MiB   (peak %.1f)\n",
                    pm.commit / mib, pm.commit_peak / mib);
    else
        std::printf("  committed by the whole process   %8.1f MiB   (peak: this "
                    "system does not report it)\n",
                    pm.commit / mib);
    std::printf("  resident now / peak              %8.1f / %.1f MiB\n",
                pm.working_set / mib, pm.working_set_peak / mib);
    std::printf("\n  Reserved is address space and costs nothing until it is\n"
                "  committed; committed is what the system has to back.  We\n"
                "  reserve once and keep what we commit, which is why our\n"
                "  per-row figures fall to zero after the first case: the memory\n"
                "  is already ours.  That is the price of the speed above.\n");
    std::printf("\nA ratio above 1.00 means this allocator is that many times\n"
                "faster than the system's.  A row marked SYSTEM is a real\n"
                "result, not noise to be explained away -- and one marked\n"
                "\"too close\" is the honest answer for a difference this\n"
                "machine cannot resolve today, not a missing one.\n");
    return 0;
}

/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file bench/bench_allocator.cpp
 * @brief This allocator against the system one, side by side on every row.
 *
 * WHAT CHANGED, AND WHY IT HAD TO.  This used to measure each row once and
 * print it.  Measured that way the hottest row -- a small block allocated and
 * freed -- came out anywhere between 1.38 and 2.35 ns/op across seven runs of
 * the SAME binary: a spread of 64%.  A benchmark with a 64% floor cannot answer
 * the question it exists for, which is whether a change to the fast path cost
 * anything; every answer it gives is noise wearing a number.  So the discipline
 * of `bench_memcpy.cpp` and `bench_memset.cpp` applies here too:
 *
 *   - **Both columns in the same round, interleaved**, order flipped on odd
 *     repeats.  If the machine slows down mid-measurement, both columns slow
 *     down together and their ratio does not notice.  Comparing two summaries
 *     measured minutes apart does notice, and calls it a result.
 *   - **Repeats summarised by the clean half**, with the spread kept.  See
 *     `support/bench_stats.h` for why a time and a ratio do not summarise the
 *     same way.
 *   - **A measured verdict floor.**  Before the tables, the same machinery runs
 *     with our allocator on BOTH sides -- so the true ratio is exactly 1.00 --
 *     and whatever it reports instead of 1.00 is what this machine cannot
 *     resolve today.  A row has to beat that, or its own spread, before it is
 *     allowed to name a winner.
 *   - **One pass per kind of core.**  On a hybrid part there is no single
 *     "speed of this machine" to report.
 *
 * THE SECOND COLUMN EXISTS AT ALL because of `support/system_alloc.h`: this
 * library replaces the C runtime's allocator in this process, so comparing
 * against it used to mean a second process and `VESTA_NO_HOST_SLAB=1`.  Loading
 * a private copy of the C runtime from disk gives back an allocator our patch
 * never touched, in the same process, callable in the same round.
 *
 * THE TOLL, and it is declared rather than hidden: the system column is reached
 * through a function pointer, because a DLL entry point is all there is.  Ours
 * is inlined, because that is what it is in a real program.  So the two columns
 * do not pay the same call overhead, and the difference would flatter us.  It
 * is measured on its own -- ours direct against ours through a pointer -- and
 * printed with the floor, so the reader can discount it instead of guessing.
 *
 * WHY THE THREADED ROWS START THE CLOCK LATE.  Creating a thread costs tens of
 * microseconds, and it used to sit inside the timed region: with 200k
 * operations per thread the setup was a large share of what was being called a
 * measurement, and it was the loudest source of noise in the whole file.  Now
 * the workers meet at a barrier and the clock starts after it, so what is timed
 * is the work.
 *
 * @code
 * ./vesta_alloc_bench_allocator                  # both columns, every pass
 * VESTA_BENCH_WORK=4 ./vesta_alloc_bench_allocator   # four times the work
 * VESTA_BENCH_PASSES=p ./vesta_alloc_bench_allocator # only the big cores
 * @endcode
 */

#include "util/alloc/host_allocator.h"
#include "util/os/os_memory.h"

#include "affinity.h"
#include "bench_stats.h"
#include "chart.h"
#include "csv.h"
#include "report.h"
#include "system_alloc.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

/// How many times each row is measured.  Odd on purpose: the summaries index a
/// median, and with an even count that is a coin flip between two samples.
constexpr int kReps = 21;

/// Multiplies every work count.  Raising it buys resolution with wall clock,
/// which is the trade the reader should be able to make without editing this.
double g_work = 1.0;

/// What this pass cannot resolve.  Measured in @c calibrate, not assumed.
double g_floor = 0.02;

/// What the pointer costs, measured the same way.  Printed, never subtracted:
/// correcting a number by another measured number hides two uncertainties
/// inside one figure that looks exact.
double g_toll = 0.0;

const char *g_pass = "";

/// Stops the optimiser from seeing that a block is never used.  Without this
/// the allocate/free pair folds away and the row measures an empty loop.
template <class T> [[gnu::always_inline]] inline void escape(T &p) noexcept {
    asm volatile("" : "+r"(p) : : "memory");
}

// =========================================================================
//  The two columns
// =========================================================================

/// The system's, reached through @c support/system_alloc.h.  Cached in plain
/// globals rather than asked for per call: `api()` is a function-local static,
/// and its guard check inside the timed loop would be measured as if it were
/// part of the allocator.
void *(*g_sys_alloc)(size_t) = nullptr;
void (*g_sys_free)(void *) = nullptr;

/// Ours, but reached the way the system's has to be reached.  Exists only to
/// price the indirection; see the file header.
[[gnu::noinline]] void *our_alloc_via_pointer(size_t n) {
    return util::host_alloc(n);
}
[[gnu::noinline]] void our_free_via_pointer(void *p) { util::host_free(p); }

void *(*g_our_alloc_ptr)(size_t) = &our_alloc_via_pointer;
void (*g_our_free_ptr)(void *) = &our_free_via_pointer;

/// This allocator, called the way a program calls it: inlined.
struct Ours {
    static const char *name() { return "ours"; }
    [[gnu::always_inline]] static void *alloc(size_t n) {
        return util::host_alloc(n);
    }
    [[gnu::always_inline]] static void release(void *p) { util::host_free(p); }
};

/// The system's.
struct Sys {
    static const char *name() { return "system"; }
    [[gnu::always_inline]] static void *alloc(size_t n) {
        return g_sys_alloc(n);
    }
    [[gnu::always_inline]] static void release(void *p) { g_sys_free(p); }
};

/// Ours through a pointer.  The control that prices the call, nothing else.
struct OursPtr {
    static const char *name() { return "ours*"; }
    [[gnu::always_inline]] static void *alloc(size_t n) {
        return g_our_alloc_ptr(n);
    }
    [[gnu::always_inline]] static void release(void *p) { g_our_free_ptr(p); }
};

// =========================================================================
//  The patterns
// =========================================================================

/// Sizes drawn from a real measurement: 81% of allocations are <= 64 bytes.
const size_t kMix[] = {16, 24, 32, 32, 48, 64, 64, 96, 128, 256};
constexpr int kMixCount = int(sizeof(kMix) / sizeof(kMix[0]));

double ns_per(Clock::time_point t0, long long ops) {
    const auto dt = Clock::now() - t0;
    const double ns = double(
        std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count());
    return ops > 0 ? ns / double(ops) : 0.0;
}

/**
 * @brief One block at a time: allocate it, free it, again.
 *
 * THE PUREST FORM OF THE FAST PATH, and the row that decides whether a change
 * to it cost anything.  There is no live set, so the same block comes straight
 * back from the free list and nothing else moves: what is left is the cache
 * lookup, the size class, and the pop and push.
 */
template <class B> double one_at_a_time(size_t size, long long pairs) {
    const auto t0 = Clock::now();
    for (long long i = 0; i < pairs; ++i) {
        void *p = B::alloc(size);
        escape(p);
        B::release(p);
    }
    return ns_per(t0, 2 * pairs);
}

/**
 * @brief Bursts: a batch allocated, then the whole batch freed.
 *
 * The compiler's usual shape, and where the size classes earn their keep.  It
 * differs from the row above in that the free lists actually grow: the block
 * that comes back is not the one just returned, so this walks memory.
 */
template <class B> double bursts(int batch, long long rounds) {
    std::vector<void *> live(static_cast<size_t>(batch));
    const auto t0 = Clock::now();
    for (long long r = 0; r < rounds; ++r) {
        for (int i = 0; i < batch; ++i)
            live[size_t(i)] = B::alloc(kMix[i % kMixCount]);
        for (int i = 0; i < batch; ++i)
            B::release(live[size_t(i)]);
    }
    return ns_per(t0, 2LL * batch * rounds);
}

/**
 * @brief One large block, allocated and freed.  Served by spans since 2026-09.
 *
 * ONE SIZE PER ROW, and it used to be all four averaged into one.  That row
 * moved 17% between repeats, and eight times the work only brought it to 14% --
 * which is how it became clear that the problem was not the sample length.  A
 * 4 KiB allocation and a 1 MiB one differ by more than two orders of magnitude,
 * so their average is dominated by the largest and moves with it; and the
 * single number it produced could not answer the question anyone actually has
 * about this path, which is WHERE the win starts.
 */
template <class B> double large(size_t size, long long rounds) {
    const auto t0 = Clock::now();
    for (long long r = 0; r < rounds; ++r) {
        void *p = B::alloc(size);
        escape(p);
        B::release(p);
    }
    return ns_per(t0, 2LL * rounds);
}

/**
 * @brief A meeting point for N threads, so the clock can start after the setup.
 *
 * Spinning rather than sleeping: the wait is a few microseconds at most, and a
 * condition variable would put the thread to sleep just before the region being
 * measured -- so the first thing every row would measure is a wake-up.
 */
class Barrier {
  public:
    explicit Barrier(unsigned n) : n_(n) {}

    void wait() {
        const unsigned g = gen_.load(std::memory_order_acquire);
        if (arrived_.fetch_add(1, std::memory_order_acq_rel) + 1 == n_) {
            arrived_.store(0, std::memory_order_relaxed);
            gen_.fetch_add(1, std::memory_order_release);
            return;
        }
        while (gen_.load(std::memory_order_acquire) == g) {
        }
    }

  private:
    std::atomic<unsigned> arrived_{0};
    std::atomic<unsigned> gen_{0};
    const unsigned n_;
};

/**
 * @brief N threads, each minding its own business.
 *
 * The point of per-thread free lists: the fast path synchronises nothing, so
 * this should scale roughly with the cores.  The clock starts AFTER the barrier
 * -- creating the threads is not what is being measured, and it used to be a
 * large enough share of the timed region to drown the result.
 */
template <class B> double threads(unsigned n, long long per_thread) {
    Barrier gate(n + 1); // the workers and this one
    std::vector<std::thread> pool;
    pool.reserve(n);
    for (unsigned t = 0; t < n; ++t)
        pool.emplace_back([&gate, per_thread] {
            gate.wait();
            for (long long i = 0; i < per_thread; ++i) {
                void *p = B::alloc(kMix[i % kMixCount]);
                escape(p);
                B::release(p);
            }
        });

    gate.wait();
    const auto t0 = Clock::now();
    for (auto &t : pool)
        t.join();
    return ns_per(t0, 2LL * per_thread * n);
}

/**
 * @brief Allocated by one thread, freed by ANOTHER.
 *
 * The case that sinks naive per-thread allocators.  Here it costs one
 * compare-exchange onto the owner's remote list and blocks nobody.
 *
 * Both phases are timed, and the threads are made before the clock starts for
 * the same reason as above.  The producer has to finish before the consumer
 * starts -- that is what makes every free a remote one -- so the barrier is
 * used twice.
 */
template <class B> double cross_thread(long long n) {
    std::vector<void *> blocks(static_cast<size_t>(n), nullptr);
    Barrier gate(2);
    std::atomic<bool> produced{false};

    std::thread worker([&] {
        gate.wait(); // the clock starts right after this
        for (long long i = 0; i < n; ++i)
            blocks[size_t(i)] = B::alloc(kMix[i % kMixCount]);
        produced.store(true, std::memory_order_release);
    });

    gate.wait();
    const auto t0 = Clock::now();
    worker.join();

    // And now the frees, from THIS thread, which did not allocate them.
    for (long long i = 0; i < n; ++i)
        B::release(blocks[size_t(i)]);
    return ns_per(t0, 2LL * n);
}

// =========================================================================
//  Rows, verdicts and the floor
// =========================================================================

/// What one row of the table knows about itself.
struct Row {
    double a = 0.0;            ///< the first column, in ns per operation
    double b = 0.0;            ///< the second
    double ratio = 0.0;        ///< b / a, measured pairwise, not divided after
    double ratio_spread = 0.0; ///< how much the ratio moved between repeats
    double time_spread = 0.0;  ///< the worse of the two columns' spreads
};

/**
 * @brief Measures two columns @c kReps times, interleaved, and summarises them.
 *
 * The ratio is computed INSIDE the loop, pairing the two samples of the same
 * repeat, and only then summarised.  Dividing two summaries instead would let
 * a slow patch of machine reach the ratio: each column absorbs it differently,
 * so the quotient of the summaries carries it while the quotient of a pair does
 * not.  It is the difference between comparing two things and comparing two
 * measurements of two things, and it shows up exactly on the rows that sit near
 * the margin -- the ones that used to change verdict between runs.
 *
 * @param first  Measures the first column once, returning ns per operation.
 * @param second The same for the second.
 */
template <class FA, class FB> Row measure_row(FA &&first, FB &&second) {
    double a[kReps], b[kReps], r[kReps];
    for (int i = 0; i < kReps; ++i) {
        if (i & 1) {
            b[i] = second();
            a[i] = first();
        } else {
            a[i] = first();
            b[i] = second();
        }
        r[i] = a[i] > 0.0 ? b[i] / a[i] : 0.0;
    }

    Row row;
    row.ratio = bench_stats::summarize_ratio(r, kReps, &row.ratio_spread);
    double na = 0.0, nb = 0.0;
    row.a = bench_stats::summarize(a, kReps, &na);
    row.b = bench_stats::summarize(b, kReps, &nb);
    row.time_spread = na > nb ? na : nb;
    return row;
}

/// The margin a row has to beat: whichever is worse, what the machine cannot
/// resolve at all or what this particular row moved.
double margin_for(const Row &r) {
    return r.ratio_spread > g_floor ? r.ratio_spread : g_floor;
}

/// One word for what the row shows, and the colour that goes with it.
const char *verdict_of(const Row &r, const char **tint) {
    if (r.ratio <= 0.0) {
        *tint = report::dim();
        return "not measured";
    }
    const double d = r.ratio > 1.0 ? r.ratio - 1.0 : 1.0 - r.ratio;
    if (d <= margin_for(r)) {
        *tint = report::amber();
        return "too close to call";
    }
    if (r.ratio > 1.0) {
        *tint = report::green();
        return "ours faster";
    }
    *tint = report::red();
    return "ours SLOWER";
}

csv::Report *g_csv = nullptr;
bool g_have_system = false;

void print_header(const char *what) {
    std::printf("\n  %-26s %10s %10s %9s  %-18s %s\n", what, "ours ns",
                "system ns", "ratio", "verdict", "spread");
    std::printf("  -------------------------- ---------- ---------- --------- "
                "------------------ ------\n");
}

void print_row(const char *section, const char *label, double bytes,
               const Row &r) {
    const char *tint = report::reset();
    const char *v = verdict_of(r, &tint);

    if (g_have_system)
        std::printf("  %-26s %10.2f %10.2f %8.2fx  %s%-18s%s %s%.0f%%%s\n",
                    label, r.a, r.b, r.ratio, tint, v, report::reset(),
                    report::dim(), r.ratio_spread * 100.0, report::reset());
    else
        std::printf("  %-26s %10.2f %10s %9s  %s%-18s%s %s%.0f%%%s\n", label,
                    r.a, "-", "-", report::dim(), "no system column",
                    report::reset(), report::dim(), r.time_spread * 100.0,
                    report::reset());

    if (g_csv != nullptr) {
        char line[256];
        std::snprintf(line, sizeof(line),
                      "\"%s\",\"%s\",%.0f,%.4f,%.4f,%.5f,%.5f,%.5f,\"%s\"",
                      section, label, bytes, r.a, r.b, r.ratio, r.ratio_spread,
                      r.time_spread, v);
        g_csv->raw(line);
    }
}

/**
 * @brief Measures what this machine cannot resolve, and what the pointer costs.
 *
 * TWO CONTROLS, and they answer different questions.
 *
 * The FLOOR runs our allocator on both sides of the same machinery.  The true
 * ratio is 1.00 by construction, so whatever comes out instead is the size of
 * the lie this machine tells today.  Every row below has to beat it.
 *
 * The TOLL runs ours inlined against ours through a function pointer.  That is
 * the handicap the system column carries and ours does not, and knowing it is
 * the difference between "we are twice as fast" and "we are twice as fast, of
 * which this much is the call".
 *
 * Both are measured once per pass: a small core does not resolve what a big one
 * does, so carrying the number over would describe the wrong machine.
 */
void calibrate() {
    const long long pairs = (long long)(400000 * g_work);

    g_floor = 0.02;
    double worst = 0.0;
    for (size_t n : {size_t(16), size_t(64), size_t(256)}) {
        const Row c = measure_row(
            [n, pairs] { return one_at_a_time<Ours>(n, pairs); },
            [n, pairs] { return one_at_a_time<Ours>(n, pairs); });
        const double d = c.ratio > 1.0 ? c.ratio - 1.0 : 1.0 - c.ratio;
        if (d > worst) worst = d;
    }
    if (worst > g_floor) g_floor = worst;

    const Row t =
        measure_row([pairs] { return one_at_a_time<Ours>(64, pairs); },
                    [pairs] { return one_at_a_time<OursPtr>(64, pairs); });
    g_toll = t.ratio;

    std::printf("\n%sVerdict margin floor, measured with this allocator on BOTH "
                "sides (so the\ntrue ratio is 1.00x): %s%.1f%%%s.  A row has to "
                "beat that, or its own spread\nif it is worse, before it names "
                "a winner.%s\n",
                report::dim(), report::bold(), g_floor * 100.0, report::dim(),
                report::reset());
    std::printf("%sThe system column is reached through a function pointer and "
                "ours is inlined.\nPriced on its own, that call costs "
                "%s%.2fx%s -- so a ratio below it is not a win.%s\n",
                report::dim(), report::bold(), g_toll, report::dim(),
                report::reset());
}

// =========================================================================
//  The sections
// =========================================================================

/// The sweep, which is also what the chart draws.
const size_t kSweep[] = {16,  24,  32,   48,   64,   96,   128,  192,
                         256, 512, 1024, 2048, 4096, 8192, 16384};
constexpr int kSweepCount = int(sizeof(kSweep) / sizeof(kSweep[0]));

double g_plot_ours[kSweepCount];
double g_plot_sys[kSweepCount];

void section_sweep() {
    report::section("one at a time",
                    "allocate a block, free it, again -- the fast path alone");
    print_header("size");

    const long long pairs = (long long)(500000 * g_work);
    for (int i = 0; i < kSweepCount; ++i) {
        const size_t n = kSweep[i];
        Row r;
        if (g_have_system)
            r = measure_row([n, pairs] { return one_at_a_time<Ours>(n, pairs); },
                            [n, pairs] { return one_at_a_time<Sys>(n, pairs); });
        else
            r = measure_row([n, pairs] { return one_at_a_time<Ours>(n, pairs); },
                            [] { return 0.0; });
        g_plot_ours[i] = r.a;
        g_plot_sys[i] = r.b;

        char label[32];
        if (n >= 1024)
            std::snprintf(label, sizeof(label), "%zu KiB", n / 1024);
        else
            std::snprintf(label, sizeof(label), "%zu bytes", n);
        print_row("one at a time", label, double(n), r);
    }
}

void section_patterns(unsigned hw) {
    report::section("patterns", "the shapes a real program actually makes");
    print_header("pattern");

    {
        const long long rounds = (long long)(2000 * g_work);
        const Row r =
            measure_row([rounds] { return bursts<Ours>(512, rounds); },
                        [rounds] {
                            return g_have_system ? bursts<Sys>(512, rounds)
                                                 : 0.0;
                        });
        print_row("patterns", "bursts of 512 small", 64, r);
    }
    /* The large sizes, one row each.  The round count comes DOWN as the size
     * goes up so that every row costs about the same wall clock: a megabyte
     * costs the system three orders of magnitude more than four kibibytes, and
     * a fixed count would make this section as slow as its worst row. */
    {
        static const struct {
            size_t size;
            long long rounds;
            const char *label;
        } kLarge[] = {
            {4096, 200000, "4 KiB"},
            {40000, 60000, "40 KiB"},
            {200000, 20000, "200 KiB"},
            {size_t(1) << 20, 6000, "1 MiB"},
        };
        for (const auto &L : kLarge) {
            const size_t size = L.size;
            const long long rounds = (long long)(double(L.rounds) * g_work);
            const Row r = measure_row(
                [size, rounds] { return large<Ours>(size, rounds); },
                [size, rounds] {
                    return g_have_system ? large<Sys>(size, rounds) : 0.0;
                });
            char label[32];
            std::snprintf(label, sizeof(label), "large, %s", L.label);
            print_row("patterns", label, double(size), r);
        }
    }

    /* BEFORE the threaded rows, on purpose.  Every thread that runs takes an
     * owner id, and while those ids are not being recycled the rows that follow
     * a wide threaded row are measured on the shared lists instead of the fast
     * path.  Putting the two-thread row first keeps that contamination at the
     * end of the section instead of in the middle of it. */
    {
        const long long n = (long long)(1000000 * g_work);
        const Row r = measure_row(
            [n] { return cross_thread<Ours>(n); },
            [n] { return g_have_system ? cross_thread<Sys>(n) : 0.0; });
        print_row("patterns", "freed by ANOTHER thread", 64, r);
    }

    const unsigned counts[] = {1, hw / 2 > 1 ? hw / 2 : 2, hw};
    for (unsigned n : counts) {
        /* Fewer operations per thread as the thread count goes up, so the row
         * costs about the same wall clock whatever the machine is: the point of
         * the row is the ns per operation, and that does not need the wide case
         * to run for a minute to be readable. */
        const long long per = (long long)(2000000 * g_work) / (n > 0 ? n : 1);
        const Row r =
            measure_row([n, per] { return threads<Ours>(n, per); },
                        [n, per] {
                            return g_have_system ? threads<Sys>(n, per) : 0.0;
                        });
        char label[48];
        std::snprintf(label, sizeof(label), "%u thread%s, each on its own", n,
                      n == 1 ? "" : "s");
        print_row("patterns", label, 64, r);
    }
}

void draw_chart() {
    if (!g_have_system) return;
    const chart::Series s[2] = {
        {"ours", g_plot_ours, report::green()},
        {"system", g_plot_sys, report::red()},
    };
    chart::plot("ns per operation, by block size", s, 2, kSweepCount, 14, 72,
                0.0, "16 B", "16 KiB");
    chart::legend();
}

/**
 * @brief Says whether the run was measured on a healthy allocator.
 *
 * The owner-id pool is 63 entries.  A run that exhausts it stops measuring the
 * fast path and starts measuring the shared lists behind the lock, and it does
 * that QUIETLY -- the numbers stay numbers.  So the count is read at the end
 * and, if it moved, the tables above are declared suspect rather than left to
 * look fine.
 */
void warn_if_starved() {
    const util::HostAllocStats s = util::host_alloc_stats();
    if (s.no_owner_id == 0) return;
    std::printf("\n%s%sTHE TABLES ABOVE ARE SUSPECT%s: the owner-id pool ran "
                "out %llu times during\nthis run.  From that point on, threads "
                "were served from the SHARED lists\nbehind the lock, so the "
                "threaded rows measured that and not the fast path.%s\n",
                report::bold(), report::red(), report::reset(),
                (unsigned long long)s.no_owner_id, report::reset());
}

} // namespace

int main() {
    report::title("allocator benchmark");

    const char *w = std::getenv("VESTA_BENCH_WORK");
    if (w != nullptr && w[0] != '\0') {
        const double v = std::atof(w);
        if (v > 0.0) g_work = v;
    }

    /* It cannot be switched off any more, so getting here means it could not
     * take over: the region failed, or a static link did not pull the objects
     * in.  Either way the "ours" column would be the system allocator under
     * another name, and the table below would compare it with itself. */
    if (!util::host_alloc_active()) {
        std::printf("\n%sThis allocator is NOT in force, so the \"ours\" column "
                    "is the system\nallocator under another name and the table "
                    "below compares it with itself.%s\n",
                    report::amber(), report::reset());
    }

    const system_alloc::Api &sys = system_alloc::api();
    g_have_system = sys.ok();
    if (g_have_system) {
        g_sys_alloc = sys.alloc;
        g_sys_free = sys.release;
        std::printf("\n%ssystem column: %s%s\n", report::dim(), sys.how,
                    report::reset());
    } else {
        std::printf("\n%sNo system column: %s.  Only this allocator is "
                    "measured.%s\n",
                    report::amber(), sys.why, report::reset());
    }

    /* Warm up.  A thread's first allocation registers it and asks the OS for a
     * chunk; timing that would measure starting up, not steady state. */
    for (int i = 0; i < 20000; ++i)
        util::host_free(util::host_alloc(64));
    if (g_have_system)
        for (int i = 0; i < 20000; ++i)
            g_sys_free(g_sys_alloc(64));

    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;

    csv::Report out("alloc", "run_id,pass,section,label,bytes,ours_ns,"
                             "system_ns,ratio,ratio_spread,time_spread,"
                             "verdict");
    g_csv = &out;

    const std::vector<affinity::Pass> passes = affinity::passes();
    if (affinity::topology().why[0] != 0)
        std::printf("\n%sOnly one pass: %s.%s\n", report::amber(),
                    affinity::topology().why, report::reset());

    for (const affinity::Pass &p : passes) {
        const bool ok = affinity::pin(p.cpus);
        const char *seen = isa::current_core_kind();

        g_pass = p.name;
        std::printf("\n%s%s#### %s ####%s\n", report::bold(), report::cyan(),
                    p.name, report::reset());
        if (!p.cpus.empty() && !ok)
            std::printf("%scould not pin to those cores; this pass measures "
                        "whatever the scheduler gave it%s\n",
                        report::amber(), report::reset());
        else if (seen[0] != '\0')
            std::printf("%srunning on a %s%s\n", report::dim(), seen,
                        report::reset());

        calibrate();
        out.begin_pass(p.name, seen, g_floor);
        section_sweep();
        section_patterns(hw);
    }
    affinity::unpin();

    draw_chart();

    const util::HostAllocStats s = util::host_alloc_stats();
    std::printf("\n%scommitted %.1f MiB in %llu chunks, %llu blocks freed by "
                "another thread%s\n",
                report::dim(), s.bytes_reserved / (1024.0 * 1024.0),
                (unsigned long long)s.chunks,
                (unsigned long long)s.remote_frees, report::reset());

    /* And what the OPERATING SYSTEM says this process cost, which is a
     * different question and the one that matters for a memory/speed
     * trade-off.  The counter above says how much was ever asked of the region,
     * not how much is still held. */
    const util::OsProcessMemory pm = util::os_process_memory();
    std::printf("%sprocess peak %.1f MiB resident, %.1f MiB now%s\n",
                report::dim(), pm.working_set_peak / (1024.0 * 1024.0),
                pm.working_set / (1024.0 * 1024.0), report::reset());

    warn_if_starved();

    if (out.open())
        std::printf("\n%sAlso written, three files sharing the prefix%s "
                    "%s%s%s.%s\n",
                    report::dim(), report::reset(), report::bold(),
                    out.prefix().c_str(), report::dim(), report::reset());
    g_csv = nullptr;
    return 0;
}

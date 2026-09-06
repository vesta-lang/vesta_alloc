/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file bench/bench_contention.cpp
 * @brief What the allocator costs when MANY THREADS use it at once.
 *
 * WHY A THIRD BENCHMARK.  `bench_allocator` and `bench_vs_malloc` both run on
 * one thread, and the whole design of this allocator -- per-thread free lists,
 * no locks on the fast path, an atomic stack for blocks freed by a stranger,
 * a per-thread span cache -- exists FOR the multi-threaded case.  A
 * single-threaded benchmark cannot see contention, and it cannot see false
 * sharing at all.
 *
 * It already paid for itself: profiling this workload found that `ThreadCache`
 * was 880 bytes with alignment 8 inside a contiguous array, so one thread's
 * counters shared a cache line with the next thread's free lists.  Machine
 * clears were 4.6% of pipeline slots; aligning the struct took them to 1.9%
 * and the workload got 7-8% faster.  None of that is visible from one thread.
 *
 * THE SHAPE OF THE WORK comes from the numbers measured on a real compilation
 * of 144k lines (`doc/PLAN_RESERVAS.md`):
 *
 *     small allocations          62,450,230
 *     freed by another thread     1,535,616   (2.5%)
 *     <= 64 B                    50,858,334   (81.1%)
 *
 * So: overwhelmingly small, almost always freed by the thread that allocated
 * it, and one in forty freed by somebody else.  That last part is what makes
 * the owner's atomic stack work, and it is the only place where threads can
 * contend.
 *
 * PASSES, LIKE THE COPY AND FILL BENCHMARKS.  On a hybrid CPU there is no
 * single answer, so the same table is measured pinned to the big cores and
 * again unpinned.  Here it matters for a reason those benchmarks do not have:
 * every thread is given the SAME number of steps, and on cores that differ by
 * 2-5x that makes a ragged tail -- the big cores finish and wait.  Unpinned,
 * that tail is a property of the machine and not of the allocator, and reading
 * it as contention is the mistake this benchmark is built to prevent.  Hence
 * the `slow/fast` column: it says outright how uneven the round was, so a
 * ragged tail is visible as a ragged tail.
 *
 * WHAT THE NUMBERS INCLUDE.  Allocating, freeing, touching the block, and the
 * mailbox.  NOT drawing the sizes -- that is done once, up front, into a table;
 * see `g_sizes` for why and for what it does not change.  Both columns pay the
 * same harness cost, so it never decided a comparison, but it did drown one:
 * with the sizes drawn inside the loop this benchmark's own noise was +-20%,
 * and without it it is +-3 to 10%.  A number is only worth as much as its
 * spread.
 */

#include "util/host_allocator.h"
#include "util/host_allocator_layout.h" // kMaxThreads: the thread budget

#include "affinity.h"
#include "report.h"

#include <algorithm>
#include <atomic>
#include <chrono>

#include <cstdio>
#include <cstdlib>

#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

/// A cheap deterministic generator.  `rand()` takes a lock in some C libraries,
/// and that lock would end up inside the measurement.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed | 1u) {}
    uint32_t next() noexcept {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return uint32_t(s >> 32);
    }
};

/// The size distribution, in percent, following the measured histogram: 81%
/// below 64 B and a short tail into the kilobytes.
struct Band {
    uint32_t upto_pct;
    uint32_t min, max;
};
const Band kBands[] = {
    {81, 8, 64},       // what dominates
    {93, 65, 256},     //
    {98, 257, 1024},   //
    {100, 1025, 8192}, // the tail
};

uint32_t draw_size(Rng &r) noexcept {
    const uint32_t p = r.next() % 100;
    for (const Band &b : kBands)
        if (p < b.upto_pct) return b.min + r.next() % (b.max - b.min + 1);
    return 64;
}

/**
 * @brief The sizes, drawn ONCE and read from a table during the measurement.
 *
 * WHY.  Drawing them inside the loop charged the allocator for the benchmark's
 * own work.  Measured with VTune on this exact workload, by function: the band
 * search and the generator behind it were 1.53 s and 0.70 s of a 17.5 s profile
 * -- about an eighth -- and they carried the worst branch prediction in the
 * whole run, 29.5% and 50.0% of their branches missed.  Those misses are the
 * benchmark deciding what to ask for; the allocator never sees them.
 *
 * WHAT THIS DOES NOT DO, which matters more than what it does: it does not make
 * the workload easier for the allocator.  The unpredictability the allocator
 * faces is in the size VALUES -- which class each request lands in -- and those
 * are drawn from exactly the same distribution as before, in the same
 * proportions.  What goes away is the harness searching its own table of bands.
 * Making the sizes themselves predictable would be measuring a program nobody
 * writes.
 *
 * A STEP AND AN OFFSET PER THREAD, and this is not decoration.  With every
 * thread walking the table the same way they would all ask for the same size on
 * the same step, and that is a different workload: synchronised pressure on one
 * size class instead of spread across them.  The step is odd so it visits every
 * entry before repeating.
 *
 * 8 KiB, which sits in L1 next to everything else.  It is not free -- it is a
 * load and it takes room in the cache -- it is just much cheaper than what it
 * replaces.
 */
constexpr uint32_t kSizeTable = 4096; // power of two: the index just masks
uint16_t g_sizes[kSizeTable];

void fill_sizes() noexcept {
    Rng r(0xD1B54A32D192ED03ull);
    for (uint32_t i = 0; i < kSizeTable; ++i)
        g_sizes[i] = uint16_t(draw_size(r));
}

/**
 * @brief Where other threads leave blocks for this one to free.
 *
 * Padded to a whole cache line ON PURPOSE.  Without it, one thread's mailbox
 * and the next one's share a line and the benchmark would be measuring its own
 * false sharing instead of the allocator's -- which is exactly the thing it
 * exists to detect.
 */
struct alignas(64) Mailbox {
    std::atomic<void *> stack{nullptr};
};

std::vector<Mailbox> g_boxes;

/**
 * @brief What one worker did in one round.  It writes only its own.
 *
 * A WHOLE CACHE LINE EACH, for the same reason as the mailboxes: two workers
 * sharing a line here would be false sharing of the BENCHMARK's own making,
 * and this is the one benchmark whose job is to find false sharing.
 *
 * THE HARNESS ITSELF SYNCHRONIZES NOTHING, and that took two corrections.  The
 * first version had the main thread busy-wait on an atomic for the length of
 * each round; VTune found it -- in a pass pinned to the big cores, 17.5 of 388
 * billion clockticks were on the SMALL ones, which is one thread spinning for
 * the whole run.  A benchmark that burns a core to wait measures a machine with
 * one core fewer, and on a pinned pass not even the cores it claims.  The
 * second version replaced the spin with a mutex and a condition variable, which
 * is worse in a subtler way: it puts a lock at the edge of the window that
 * measures lock-free code.
 *
 * So there is no barrier at all.  Each round creates its threads and joins
 * them, and `join` is the OS blocking primitive -- no spin, no lock of ours.
 * Each worker times ITSELF, so neither creating nor joining is inside anything
 * that gets reported.
 *
 * Creating threads per round used to be impossible here: the allocator handed
 * a thread its own free lists and never took them back, so the rounds burned
 * through the sixty-four identifiers and everything fell to the shared lists
 * behind the lock -- 1.50 ns per operation before the cliff and 438 after, in
 * the same run.  Now the identifiers come back when a thread dies, which is
 * also why the state that matters SURVIVES the round: the free lists and the
 * chunks are inherited by whoever takes the identifier next.  That makes this
 * benchmark its own regression test for the recycling -- if it ever breaks
 * again, these numbers collapse by two orders of magnitude.
 */
struct alignas(64) Slot {
    Clock::time_point begin, end;
    uint64_t ops;
};
std::vector<Slot> g_slots;

void push(Mailbox &b, void *p) noexcept {
    void *old = b.stack.load(std::memory_order_relaxed);
    do {
        *reinterpret_cast<void **>(p) = old;
    } while (!b.stack.compare_exchange_weak(old, p, std::memory_order_release,
                                            std::memory_order_relaxed));
}

/**
 * @brief The two allocators, chosen by TYPE.
 *
 * A number with nothing beside it says nothing: "1.39 ns per operation" is only
 * meaningful next to what the system charges for the same work.  And the choice
 * has to be a type resolved at compile time, not a pointer or a flag -- with an
 * indirect call in the loop, both columns would be measuring the indirect call
 * as much as the allocator, and the one that is otherwise five instructions
 * would suffer most.
 */
struct Ours {
    static void *alloc(size_t n) noexcept { return util::host_alloc(n); }
    static void free(void *p) noexcept { util::host_free(p); }
    static const char *name() noexcept { return "ours"; }
};
struct System {
    static void *alloc(size_t n) noexcept { return std::malloc(n); }
    static void free(void *p) noexcept { std::free(p); }
    static const char *name() noexcept { return "malloc"; }
};

template <class A> void drain(Mailbox &b) noexcept {
    void *p = b.stack.exchange(nullptr, std::memory_order_acquire);
    while (p != nullptr) {
        void *next = *reinterpret_cast<void **>(p);
        A::free(p);
        p = next;
    }
}

/// One round of the workload.  Split out of `worker` so the loop the profiler
/// has to attribute is only the allocator and nothing else.
template <class A>
void round(unsigned id, unsigned threads, int steps, Rng &r,
           std::vector<void *> &live) {
    const size_t n_live = live.size();
    /* Its own walk of the size table, offset and step: see `g_sizes`. */
    uint32_t si = (id * 2654435761u) & (kSizeTable - 1);
    const uint32_t stride = 2u * id + 1u;
    for (int i = 0; i < steps; ++i) {
        const size_t slot = r.next() % n_live;
        void *old = live[slot];
        if (old != nullptr) {
            /* One in forty goes to ANOTHER thread.  It is the 2.5% measured on
             * a real compilation, and the only thing that makes the owner's
             * atomic stack do any work.  Forty, not a power of two: rounding it
             * to 1-in-64 to save a division would quietly measure 1.6%. */
            if (r.next() % 40u == 0u && threads > 1) {
                unsigned other = r.next() % threads;
                if (other == id) other = (other + 1) % threads;
                push(g_boxes[other], old);
            } else {
                A::free(old);
            }
        }
        void *p = A::alloc(g_sizes[si]);
        si = (si + stride) & (kSizeTable - 1);
        if (p != nullptr) *static_cast<volatile char *>(p) = 1;
        live[slot] = p;

        // Every so often, collect what the others left.
        if ((i & 255) == 0) drain<A>(g_boxes[id]);
    }
    drain<A>(g_boxes[id]);
}

/// @param cpus  Cores this pass is pinned to; empty means wherever it lands.
template <class A>
void worker(unsigned id, unsigned threads, int steps,
            const std::vector<int> *cpus) {
    if (cpus != nullptr) affinity::pin(*cpus);

    Rng r(0x9E3779B97F4A7C15ull + id * 0x1000193ull);
    constexpr int kLive = 512;
    std::vector<void *> live(kLive, nullptr);

    /* The clock starts AFTER pinning and after the vector exists: creating the
     * thread and placing it is not what is being measured. */
    g_slots[id].begin = Clock::now();
    round<A>(id, threads, steps, r, live);
    g_slots[id].end = Clock::now();
    g_slots[id].ops = uint64_t(steps);

    for (void *p : live)
        A::free(p);
}

/// What one round cost, the ways it can be read.
struct Sample {
    double per_op;   ///< wall time over every operation: aggregate throughput
    double per_core; ///< what one operation costs on one core
    double ms;       ///< wall time of the round
    /**
     * @brief Slowest worker over fastest, in the same round.
     *
     * WHY IT IS WORTH A COLUMN.  Every worker is given the SAME number of
     * steps, so with equal treatment they should all take about the same time
     * and this is 1.0.  When it is not, the aggregate throughput has an
     * explanation that no average can show: the round lasts as long as the
     * slowest one, and one thread being slow for its own reasons is a very
     * different fault from every thread being slow.
     *
     * It earned its place immediately: on Linux the wall clock said 285 ms
     * while the average worker spent 107, and this number said which of the
     * two kinds of problem that was.
     */
    double spread;
};

/// Creates the threads, waits with `join`, and reads the workers' own clocks.
template <class A>
Sample one_round(const affinity::Pass &p, unsigned threads, int steps) {
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (unsigned i = 0; i < threads; ++i)
        pool.emplace_back(worker<A>, i, threads, steps,
                          p.cpus.empty() ? nullptr : &p.cpus);
    for (auto &t : pool)
        t.join();

    /* THE MAILBOXES MUST BE EMPTY BEFORE THIS RETURNS, and it is not tidiness:
     * a block pushed into somebody's mailbox after their last drain has nobody
     * left to collect it, and the next round may belong to the OTHER allocator
     * -- which would hand a block of ours to `free`.  That is not a leak, it is
     * heap corruption, and it is what this benchmark did the moment it grew a
     * second column.  Every worker has joined, so draining here is complete. */
    for (Mailbox &b : g_boxes)
        drain<A>(b);

    /* Both numbers come from the workers' OWN clocks, so neither creating them
     * nor joining them is inside either one.  The wall is from the first that
     * started to the last that finished; the per-core cost is the plain sum of
     * what each of them spent. */
    Clock::time_point first = g_slots[0].begin, last = g_slots[0].end;
    double busy = 0.0, slowest = 0.0, fastest = 0.0;
    uint64_t ops = 0;
    for (const Slot &s : g_slots) {
        if (s.begin < first) first = s.begin;
        if (s.end > last) last = s.end;
        const double own = double(
            std::chrono::duration_cast<std::chrono::nanoseconds>(s.end - s.begin)
                .count());
        busy += own;
        if (own > slowest) slowest = own;
        if (fastest == 0.0 || own < fastest) fastest = own;
        ops += s.ops;
    }
    Sample out{0.0, 0.0, 0.0, 0.0};
    if (ops == 0) return out;
    const double wall = double(
        std::chrono::duration_cast<std::chrono::nanoseconds>(last - first)
            .count());
    out.per_op = wall / double(ops);
    out.per_core = busy / double(ops);
    out.ms = wall / 1e6;
    out.spread = fastest > 0.0 ? slowest / fastest : 0.0;
    return out;
}

uint64_t g_ops_per_round = 0;

/**
 * @brief Which of the two columns to run.
 *
 * BOTH is the answer to "which is faster" and the default.  One alone is for
 * PROFILING: with the system allocator eight times slower here, it takes about
 * seven eighths of the samples, so a profile of the pair says a great deal
 * about the system's allocator and very little about ours.  Asking for one is
 * how the profile gets pointed at the thing being worked on -- and the ratio
 * is not printed then, because with nothing to compare against it would be
 * inventing one.
 */
enum class Only { Both, Ours, System };
Only g_only = Only::Both;

/**
 * @brief Prints one allocator's row and returns its median per-core cost.
 *
 * TWO COLUMNS, because either one alone invites a wrong reading:
 *
 *   per op      wall time divided by every operation every thread did.  It is
 *               AGGREGATE THROUGHPUT, so it improves just by adding threads --
 *               comparing a 16-thread pass with a 24-thread one by this column
 *               says which pass had more cores, not which allocator is better.
 *   per core    the sum of what each worker actually spent, over the same
 *               operations: what one operation costs on one core.  This is the
 *               one comparable ACROSS passes, and the one that gets worse when
 *               threads fight.
 *
 * The MEDIAN is what is reported.  The mean would be dragged by a single round
 * that landed on a busy machine, and the minimum describes a lucky run rather
 * than the workload.  The spread is printed next to it so that a difference
 * smaller than the noise is visible as such instead of read as a result.
 */
double report_row(const char *name, std::vector<Sample> v) {
    std::vector<double> per_op, per_core, spread;
    per_op.reserve(v.size());
    per_core.reserve(v.size());
    spread.reserve(v.size());
    double best_ms = 0.0;
    for (const Sample &s : v) {
        per_op.push_back(s.per_op);
        per_core.push_back(s.per_core);
        spread.push_back(s.spread);
        if (best_ms == 0.0 || s.ms < best_ms) best_ms = s.ms;
    }
    std::sort(per_op.begin(), per_op.end());
    std::sort(per_core.begin(), per_core.end());
    std::sort(spread.begin(), spread.end());
    const size_t mid = per_op.size() / 2;
    const double noise =
        per_op.front() > 0.0
            ? 100.0 * (per_op.back() - per_op.front()) / per_op.front()
            : 0.0;

    std::printf("      %s%-8s%s %7.1f ms   %s%5.2f ns%s/op  %s%6.2f ns%s/op/core"
                "   slow/fast %s%4.1fx%s   %s+-%.1f%%%s\n",
                report::bold(), name, report::reset(), best_ms, report::green(),
                per_op[mid], report::reset(), report::cyan(), per_core[mid],
                report::reset(),
                spread[mid] > 1.5 ? report::amber() : report::dim(), spread[mid],
                report::reset(), noise > 5.0 ? report::amber() : report::dim(),
                noise, report::reset());
    return per_core[mid];
}

void run_pass(const affinity::Pass &p, int steps, int reps, unsigned want) {
    /* As many threads as cores in the pass, unless asked otherwise.  With more,
     * they take turns and the number starts describing the scheduler as much as
     * us -- which is exactly what one particular question needs to see, so it
     * can be asked for, but it is never the default.
     *
     * THE QUESTION IT ANSWERS: the allocator can name 63 owners at a time, and
     * a thread past that is served from the shared lists, behind the only lock
     * there is.  Whether that lock is worth removing cannot be settled by
     * reading the code -- it has to be measured with more live threads than
     * identifiers, repeated, because a single run at that thread count varies
     * by more than the effect. */
    unsigned threads = want;
    if (threads == 0) {
        threads = unsigned(p.cpus.size());
        if (threads == 0) {
            threads = std::thread::hardware_concurrency();
            if (threads == 0) threads = 8;
        }
    }

    g_boxes = std::vector<Mailbox>(threads);
    g_slots = std::vector<Slot>(threads);

    g_ops_per_round = uint64_t(steps) * threads;

    /* A warm-up round for EACH of the two, not counted.  It is where the chunks
     * get committed for the first time, and charging that to whichever went
     * first would be charging one of them for being first.  What it warms
     * SURVIVES the round even though the threads do not: the free lists go back
     * with the identifier and the next round inherits them. */
    if (g_only != Only::System) one_round<Ours>(p, threads, steps);
    if (g_only != Only::Ours) one_round<System>(p, threads, steps);

    /* INTERLEAVED AND IN BOTH ORDERS: ours, system, system, ours.  Two runs one
     * after the other cannot tell a difference from the machine drifting --
     * thermal, another process, the scheduler settling -- and the drift is not
     * symmetric, so whichever goes first pays for it.  Running A-B-B-A inside
     * every repetition puts each of them equally often in each position. */
    std::vector<Sample> ours, sys;
    for (int rd = 0; rd < reps; ++rd) {
        if (g_only == Only::System) {
            sys.push_back(one_round<System>(p, threads, steps));
            sys.push_back(one_round<System>(p, threads, steps));
            continue;
        }
        if (g_only == Only::Ours) {
            ours.push_back(one_round<Ours>(p, threads, steps));
            ours.push_back(one_round<Ours>(p, threads, steps));
            continue;
        }
        const Sample a1 = one_round<Ours>(p, threads, steps);
        const Sample b1 = one_round<System>(p, threads, steps);
        const Sample b2 = one_round<System>(p, threads, steps);
        const Sample a2 = one_round<Ours>(p, threads, steps);
        ours.push_back(a1);
        ours.push_back(a2);
        sys.push_back(b1);
        sys.push_back(b2);
    }

    if (ours.empty() && sys.empty()) return;

    std::printf("  %s%s%s  %u threads, %llu operations a round\n", report::bold(),
                p.name, report::reset(), threads,
                (unsigned long long)g_ops_per_round);
    const double a = ours.empty() ? 0.0 : report_row("ours", ours);
    const double b = sys.empty() ? 0.0 : report_row("malloc", sys);
    if (a > 0.0 && b > 0.0)
        std::printf("      %s%-8s%s %s%.2fx%s\n", report::dim(), "ratio",
                    report::reset(), b / a >= 1.0 ? report::green() : report::red(),
                    b / a, report::reset());
}

} // namespace

int main(int argc, char **argv) {
    const int steps = argc > 1 ? std::atoi(argv[1]) : 5000000;
    const int reps = argc > 2 ? std::atoi(argv[2]) : 5;
    /// Zero -- the default -- means one thread per core of the pass.
    const unsigned want = argc > 3 ? unsigned(std::atoi(argv[3])) : 0u;
    if (argc > 4) {
        const char *w = argv[4];
        if (w[0] == 'o') g_only = Only::Ours;
        else if (w[0] == 'm') g_only = Only::System;
    }

    std::printf("== the allocator with many threads at once ==\n\n");
    std::printf("Each thread keeps 512 blocks alive and replaces one at random.\n"
                "Sizes follow the histogram measured on a real compilation, and\n"
                "one free in forty is handed to another thread -- the only place\n"
                "where threads can contend.\n");
    if (!util::host_alloc_active()) {
        std::printf("\n%sThe allocator is OFF (VESTA_NO_HOST_SLAB): this measures\n"
                    "the system one instead.%s\n",
                    report::amber(), report::reset());
    }

    const std::vector<affinity::Pass> passes = affinity::passes();
    if (affinity::topology().why[0] != 0)
        std::printf("\n%sOnly one pass: %s.%s\n", report::amber(),
                    affinity::topology().why, report::reset());

    /* The sizes are drawn HERE, once, outside everything that gets measured. */
    fill_sizes();

    const util::HostAllocStats before = util::host_alloc_stats();
    std::printf("\n%s%d steps per thread, median of %d (plus a warm-up)%s\n\n",
                report::dim(), steps, reps, report::reset());
    for (const affinity::Pass &p : passes)
        run_pass(p, steps, reps, want);

    /* DID THE WORKLOAD ACTUALLY DO WHAT IT CLAIMS?  The cross-thread frees are
     * the whole reason this benchmark exists, and they are also the easiest
     * thing to lose by accident -- one wrong constant in the size table or in the
     * mailbox and every free would be local, the numbers would look fine, and
     * the contention path would not have been measured at all.  So the share
     * is printed and compared against the 2.5% it is imitating. */
    const util::HostAllocStats a = util::host_alloc_stats();
    const uint64_t local = a.small_frees - before.small_frees;
    const uint64_t remote = a.remote_frees - before.remote_frees;
    const double pct =
        local + remote == 0 ? 0.0 : 100.0 * double(remote) / double(local + remote);
    std::printf("\n  %sfreed by another thread: %.2f%% (%llu of %llu) -- the "
                "shape being imitated is 2.5%%%s\n",
                pct >= 1.0 && pct <= 5.0 ? report::dim() : report::amber(), pct,
                (unsigned long long)remote, (unsigned long long)(local + remote),
                report::reset());

    /* WAS THIS THE FAST PATH AT ALL?  The allocator can name a limited number
     * of owners at once, and a thread past that is served from the shared
     * lists behind the only lock there is.  Asking for more threads than owners
     * is a legitimate measurement -- it is the only way to find out what that
     * lock costs -- but reading it as the fast path is not, so the allocator is
     * asked directly instead of the answer being assumed from the thread count.
     * It is not the same question: threads that finish before the last ones
     * start never overlap, so a high thread count does NOT imply the limit was
     * ever reached. */
    const uint64_t no_id = a.no_owner_id - before.no_owner_id;
    if (no_id != 0)
        std::printf("  %sran out of owner ids %llu times: those threads used "
                    "the SHARED lists, behind the lock.  This is the fallback "
                    "being measured, not the fast path.%s\n",
                    report::amber(), (unsigned long long)no_id, report::reset());
    else if (want >= util::kMaxThreads)
        std::printf("  %s%u threads asked for and the owner ids never ran out: "
                    "they did not overlap, so this did NOT measure the "
                    "fallback.%s\n",
                    report::amber(), want, report::reset());

    if (passes.size() > 1)
        std::printf("\n%sCompare builds on the PINNED pass: every thread gets\n"
                    "the same number of steps, so on cores that differ by 2-5x\n"
                    "the unpinned pass ends with a ragged tail and its average\n"
                    "says more about the scheduler than about the allocator.%s\n",
                    report::dim(), report::reset());
    return 0;
}

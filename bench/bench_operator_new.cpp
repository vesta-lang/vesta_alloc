/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file bench/bench_operator_new.cpp
 * @brief What `operator new` costs, and what recording call sites adds to it.
 *
 * WHY A SEPARATE BENCHMARK.  The other ones call `host_alloc` directly, which
 * is the right thing when what you are measuring is the allocator.  But the
 * call-site patch does not touch `host_alloc`: it rewrites the entry of
 * `operator new`.  Measuring through `host_alloc` would show nothing at all --
 * and "nothing changed" is exactly the answer you must not get by accident.
 *
 * THREE MODES, one per process, because the patch cannot be undone -- it writes
 * over code, so switching it off mid-run would mean writing over code again
 * while the program is using it:
 *
 *   off      `operator new` untouched.  This is what everyone who is not
 *            measuring pays, and the number that has to stay flat.
 *   patched  the jump is installed but recording is off.  Isolates the cost of
 *            the detour on its own.
 *   record   patched and recording.  The full price of knowing who allocates.
 *
 * Run it three times, interleaved and in both orders; a single pass in a fixed
 * order cannot tell a real difference from the machine drifting.
 *
 *     bench_operator_new off
 *     bench_operator_new patched
 *     VESTA_HOST_ALLOC_SITES=1 bench_operator_new record
 *
 * `record` NEEDS the environment variable; it does not flip `g_measure` by
 * hand.  That was tried and it is wrong: the library reads the variable BEFORE
 * the first allocation on purpose, and switching it on later leaves everything
 * from start-up out.  Measured, the difference is not subtle -- flipping it by
 * hand recorded 1 allocation out of 10,000,000 while still paying most of the
 * cost, so the benchmark was timing the work of recording and counting none of
 * it.  Asking for it the way a user would is also the only way to measure what
 * a user pays.
 *
 * EVERY loop in here goes through `alloc_free`, which parks the pointer in a
 * `volatile`.  Not a precaution: at `-O2` GCC deleted a warm-up loop that did
 * not have it, and the benchmark then reported that measurement was off for a
 * run that recorded ten million allocations without a hitch.
 */

#include "util/alloc_sites.h"
#include "util/call_site.h"
#include "util/host_allocator.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <new>

namespace {

using Clock = std::chrono::steady_clock;

/* Sizes that stay on the small path, which is the one the patch sits in front
 * of.  Cycling through several rather than repeating one keeps a single size
 * class from being the whole answer. */
constexpr std::size_t kSizes[] = {16, 24, 32, 48, 64, 96, 128, 192};
constexpr unsigned kSizeCount = sizeof(kSizes) / sizeof(kSizes[0]);

constexpr unsigned kWarmUp = 100000;
constexpr unsigned kRounds = 5;
constexpr unsigned kPerRound = 2000000;

/**
 * @brief Where the pointer goes so the pair cannot be optimised away.
 *
 * Since C++14 the compiler is allowed to DELETE an unused `new`/`delete` pair,
 * and it does: at `-O2` GCC removed a whole 100,000-iteration warm-up loop that
 * lacked this, leaving a benchmark that timed an empty loop and a self-check
 * that read the allocator's state before the allocator had ever run.
 */
void *volatile g_sink;

/**
 * @brief One allocate/free pair.  EVERY loop here goes through this.
 *
 * Allocate and free straight away on purpose: what is being timed is the entry
 * into `operator new`, not the allocator's behaviour when memory piles up --
 * that is what the other benchmarks are for.
 *
 * One function rather than the same three lines written twice, because the
 * `volatile` is not decoration: a copy without it is a loop the compiler is
 * free to delete, and that deletion is silent.
 */
inline void alloc_free(unsigned i) {
    void *p = ::operator new(kSizes[i & (kSizeCount - 1)]);
    g_sink = p;
    ::operator delete(p);
}

/// One round of allocate/free, returning nanoseconds per pair.
double one_round() {
    const auto t0 = Clock::now();
    for (unsigned i = 0; i < kPerRound; ++i) alloc_free(i);
    const auto dt = Clock::now() - t0;
    const double ns =
        double(std::chrono::duration_cast<std::chrono::nanoseconds>(dt)
                   .count());
    return ns / double(kPerRound);
}

} // namespace

/**
 * @brief Dumps the first bytes of a function, so the patch can be SEEN.
 *
 * Printing the entry before and after installing turns "the patch went in" from
 * something the code claims into something you can read.  It is what tells a
 * timing that did not move because the detour is cheap apart from one that did
 * not move because nothing was ever written.
 */
void dump_entry(const char *what, const void *fn) {
    const unsigned char *p = static_cast<const unsigned char *>(fn);
    std::printf("  %-8s %p:", what, fn);
    for (unsigned i = 0; i < 8; ++i) std::printf(" %02x", p[i]);
    if (p[0] == 0xE9) {
        std::int32_t rel = 0;
        std::memcpy(&rel, p + 1, sizeof(rel));
        const unsigned char *dst = p + 5 + rel;
        std::printf("   jmp -> %p", static_cast<const void *>(dst));
    }
    std::printf("\n");
}

const void *plain_new_entry() {
    return reinterpret_cast<const void *>(
        static_cast<void *(*)(std::size_t)>(&::operator new));
}

int main(int argc, char **argv) {
    const char *mode = (argc > 1) ? argv[1] : "off";

    const bool patch = std::strcmp(mode, "off") != 0;
    const bool record = std::strcmp(mode, "record") == 0;

    dump_entry("before", plain_new_entry());
    if (patch && !util::install_call_site_patch()) {
        std::printf("bench_operator_new: the patch could not be installed\n");
        return 1;
    }
    dump_entry("after", plain_new_entry());

    /* Warm up: the first allocation of a thread builds its cache, and that
     * one-off cost has nothing to do with what is being compared.  It also
     * initialises the allocator, which the check below depends on -- so it goes
     * through `alloc_free`, where the `volatile` keeps the compiler from
     * deleting the loop.  It already deleted it once. */
    for (unsigned i = 0; i < kWarmUp; ++i) alloc_free(i);

    /* `record` mode only means something if measurement was asked for the
     * proper way, from the environment, before anything was allocated.  Saying
     * so and stopping beats producing a number for something that never
     * happened.
     *
     * Checked AFTER the warm-up on purpose: the allocator reads the environment
     * when it initialises, and it initialises on the FIRST ALLOCATION.  Asking
     * before that would find the flag off and reject a perfectly good run --
     * and that is not hypothetical.  When the warm-up loop had no `volatile`
     * sink, GCC deleted it, no allocation happened before this line, and the
     * check reported "measurement is off" about a run that went on to record
     * ten million allocations perfectly.  A self-check that accuses a working
     * run is worse than no self-check: it sends whoever reads it after a bug
     * that is not there. */
    {
        const char *env = std::getenv("VESTA_HOST_ALLOC_SITES");
        std::printf("  measure  %s   (env=%s)\n",
                    util::detail::g_measure ? "on" : "off",
                    env ? env : "<unset>");
    }
    if (record && !util::detail::g_measure) {
        std::printf(
            "bench_operator_new: `record` needs VESTA_HOST_ALLOC_SITES=1 in "
            "the environment.  The library reads it before the first "
            "allocation on purpose, so turning it on from here would measure "
            "the work of recording while recording almost nothing -- measured, "
            "1 allocation out of 10,000,000.\n"
            "  If the line above says the variable IS set and measurement is "
            "still off, the fault is here and not in the allocator: nothing had "
            "been allocated by the time it was asked, so the allocator had not "
            "read the environment yet.  Check that the warm-up loop above still "
            "survives optimisation.\n");
        return 1;
    }

    double best = 0.0;
    for (unsigned r = 0; r < kRounds; ++r) {
        const double ns = one_round();
        if (r == 0 || ns < best) best = ns;
    }
    /* Measurement is left exactly as it was found: the environment asked for
     * it, so it is not this benchmark's to switch off. */

    /* The BEST of the rounds, not the average.  Anything else the machine does
     * only ever adds time, so the fastest round is the one least polluted --
     * and this is a comparison between modes, where a run that got interrupted
     * would land entirely on the difference. */
    std::printf("%-8s %7.2f ns per new/delete pair", mode, best);

    /* And, in `record` mode, how many sites actually got recorded.
     *
     * Without this the benchmark can report a beautiful number for measuring
     * something it never measured: if recording silently did nothing -- a
     * failed patch, a table that could not be reserved -- the timing would come
     * out identical to `off` and read as "recording is free".  A benchmark that
     * cannot tell those two apart is worse than no benchmark. */
    if (record) {
        util::AllocSite sites[64];
        const unsigned n = util::alloc_sites_snapshot(sites, 64);
        unsigned long long total = 0;
        for (unsigned i = 0; i < n; ++i) total += sites[i].count;
        std::printf("   (%u sites, %llu recorded allocations)", n, total);
        if (n == 0)
            std::printf("  <-- NOTHING WAS RECORDED: the number above is not "
                        "the cost of measuring");
    }
    std::printf("\n");
    return 0;
}

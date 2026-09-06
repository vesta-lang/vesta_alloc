/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: GPLv2 + runtime exception (see LICENSE).
 */

/**
 * @file bench/bench_reserve_cost.cpp
 * @brief What reserving address space actually costs, on THIS machine.
 *
 * WHY THIS EXISTS.  "Reserving address space is free on 64-bit" is repeated
 * everywhere and it is not quite true: the OS still charges page-table
 * bookkeeping.  On the machine this allocator was tuned on it came out at about
 * 2.7 MiB of commit and 0.5 ms per TiB reserved -- and that half-millisecond
 * lands in process STARTUP, which is a cost this project cares about.
 *
 * The region size in `util/host_allocator_layout.h` was chosen from this table.
 * Rather than leaving that as a number measured on someone else's computer,
 * this benchmark re-derives it wherever it runs.
 *
 * It also answers the question the allocator asks at startup: how much can this
 * machine actually reserve?  `util::os_reserve_largest` does not assume -- it
 * asks -- and this shows what the answer looks like.
 */

#include "util/os_memory.h"

#include <chrono>
#include <cstdint>
#include <cstdio>

namespace {

double ms_since(std::chrono::steady_clock::time_point t0) {
    const auto dt = std::chrono::steady_clock::now() - t0;
    return double(
               std::chrono::duration_cast<std::chrono::microseconds>(dt).count()) /
           1000.0;
}

void row(const char *name, size_t bytes) {
    const util::OsProcessMemory before = util::os_process_memory();

    const auto t0 = std::chrono::steady_clock::now();
    void *p = util::os_reserve(bytes);
    const double reserve_ms = ms_since(t0);

    if (p == nullptr) {
        std::printf("  %-9s  could NOT be reserved\n", name);
        return;
    }

    const util::OsProcessMemory after = util::os_process_memory();

    // Commit and touch one page in the MIDDLE, to check that reaching far into
    // a huge reservation is not more expensive than reaching into a small one.
    void *mid = static_cast<char *>(p) + bytes / 2;
    const auto t1 = std::chrono::steady_clock::now();
    const bool ok = util::os_commit(mid, 64 * 1024);
    if (ok) *static_cast<volatile char *>(mid) = 1;
    const double touch_ms = ms_since(t1);

    std::printf("  %-9s  %8.3f ms   %8.3f ms   %+8.0f KiB  %+8.0f KiB\n", name,
                reserve_ms, touch_ms,
                double(after.working_set - before.working_set) / 1024.0,
                double(after.commit - before.commit) / 1024.0);

    util::os_release(p, bytes);
}

} // namespace

int main() {
    const size_t GiB = size_t(1) << 30;

    std::printf("== cost of reserving address space ==\n\n");

    const util::OsSystemMemory sys = util::os_system_memory();
    std::printf("this machine: %.1f GiB of RAM (%.1f free), "
                "%.0f GiB addressable\n",
                double(sys.physical_total) / double(GiB),
                double(sys.physical_available) / double(GiB),
                double(sys.address_space_total) / double(GiB));
    std::printf("page %zu bytes, reservations aligned to %zu bytes\n\n",
                util::os_page_size(), util::os_reserve_granularity());

    std::printf("  %-9s  %11s   %11s   %12s  %12s\n", "size", "reserve",
                "commit+touch", "working set", "commit");
    std::printf("  ---------  -----------   -----------   ------------  "
                "------------\n");

    if (sizeof(void *) >= 8) {
        const struct {
            const char *name;
            size_t bytes;
        } sizes[] = {{"16 GiB", 16 * GiB},     {"64 GiB", 64 * GiB},
                     {"256 GiB", 256 * GiB},   {"1 TiB", 1024 * GiB},
                     {"4 TiB", 4096 * GiB},    {"16 TiB", 16384 * GiB}};
        for (const auto &s : sizes)
            row(s.name, s.bytes);
    } else {
        // On 32-bit there is no room for any of that.
        row("64 MiB", 64u << 20);
        row("256 MiB", 256u << 20);
        row("1 GiB", 1024u << 20);
    }

    // And what the allocator itself would get if it asked right now.
    std::printf("\n");
    size_t got = 0;
    void *base = util::os_reserve_largest(
        sizeof(void *) >= 8 ? 256 * GiB : (size_t(256) << 20),
        size_t(64) << 20, &got);
    if (base == nullptr) {
        std::printf("os_reserve_largest: could not get even the 64 MiB floor\n");
    } else {
        std::printf("os_reserve_largest(256 GiB, floor 64 MiB) -> %.1f GiB\n",
                    double(got) / double(GiB));
        util::os_release(base, got);
    }

    std::printf("\nReading this table: pick a region big enough that address\n"
                "space stops being the binding constraint, and small enough\n"
                "that the startup cost stays invisible.  Past a point the\n"
                "limit is physical memory anyway, and reserving more buys\n"
                "nothing.\n");
    return 0;
}

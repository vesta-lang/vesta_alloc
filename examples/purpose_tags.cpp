/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file examples/purpose_tags.cpp
 * @brief Saying WHAT an allocation is for, and reading the breakdown.
 *
 * The problem this solves: a bump arena serves an allocation five times faster
 * than a general allocator (1.95 ns against 10.03), but only for allocations
 * that die soon and never grow.  And all `operator new` ever receives is a
 * byte count -- a `std::string` has no way to state its purpose.
 *
 * A tag states it by SCOPE: wrap a phase and everything allocated inside is
 * counted, without touching a single call site.
 *
 * Mind the threads: a scope applies to ITS thread.  If the work is farmed out,
 * the tag has to be carried across -- see the end of this example.  Without
 * that, whatever the workers allocate shows up as "unknown", which is a FALSE
 * data point, not a missing one.
 */

#include "util/alloc/host_allocator.h"

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

/// Some "phase" that allocates in more than one way.
void work(int n) {
    std::vector<std::string> v;
    for (int i = 0; i < n; ++i)
        v.push_back("item number " + std::to_string(i));
}

void show(const char *title, const util::HostAllocStats &s) {
    std::printf("\n%s\n", title);
    for (uint32_t g = 0; g < util::AllocTag::kSlots; ++g) {
        if (s.by_tag[g] == 0) continue;
        std::printf("    %-18s %10llu  %5.1f%%\n",
                    util::alloc_tag_name(util::AllocTag::from_raw((uint8_t)g)),
                    (unsigned long long)s.by_tag[g],
                    100.0 * double(s.by_tag[g]) / double(s.small_allocs));
    }
}

} // namespace

int main() {
    std::printf("== purpose tags ==\n");
    if (!util::host_alloc_active()) {
        std::printf("allocator is off; there is no breakdown to show\n");
        return 0;
    }

    // 1. Saying nothing: it all lands in "unknown", the honest default.
    work(200);

    // 2. A phase that states its purpose.  Everything inside is counted there,
    //    including the `std::string`s, which cannot state anything themselves.
    {
        const util::AllocScope phase{
            {util::AllocUse::Medium, util::AllocShape::Growing}};
        work(200);
    }

    // 3. A different one, to show that scopes nest and restore.
    {
        const util::AllocScope phase{
            {util::AllocUse::Instant, util::AllocShape::Fixed}};
        for (int i = 0; i < 200; ++i)
            util::host_free(util::host_alloc(48));
    }

    // 4. With threads: the tag does NOT travel on its own.  Read it on the
    //    thread that hands out the work and re-apply it on the one that does
    //    it.  This is exactly what the compiler's pool does in
    //    `ir/parallel_for.cpp`.
    {
        const util::AllocScope phase{
            {util::AllocUse::Long, util::AllocShape::Fixed}};
        const util::AllocTag parent_tag = util::AllocScope::current();

        std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i)
            threads.emplace_back([parent_tag] {
                const util::AllocScope inherited{parent_tag};
                work(100);
            });
        for (auto &t : threads)
            t.join();
    }

    show("breakdown by purpose:", util::host_alloc_stats());
    std::printf("\nWhatever shows up as \"unknown\" is, literally, what does\n"
                "not declare anything yet: the list of what is left to do.\n");
    return 0;
}

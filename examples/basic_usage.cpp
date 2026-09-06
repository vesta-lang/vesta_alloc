/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file examples/basic_usage.cpp
 * @brief The minimum: allocate, free, and look at what happened.
 *
 * Three things worth seeing together:
 *
 *  1. **You do not have to call anything.**  Just linking the library makes
 *     every `new` and every `std::string` in the program go through it.  That
 *     is what makes it worth doing: a program's allocations are not
 *     concentrated in any one place, so you only win by touching them all at
 *     once.
 *  2. **But you can call it**, from C++ and from C.
 *  3. **And you can look**, which is what separates an allocator from a black
 *     box.
 */

#include "util/host_allocator.h"
#include "util/host_allocator_c.h"

#include <cstdio>
#include <string>
#include <vector>

int main() {
    std::printf("== basic usage ==\n\n");

    if (!util::host_alloc_active()) {
        std::printf("The allocator is OFF (VESTA_NO_HOST_SLAB).\n"
                    "Everything goes to the system one; the program still "
                    "works.\n");
        return 0;
    }

    const util::HostAllocStats before = util::host_alloc_stats();

    // 1. Without calling anything: this already goes through the allocator.
    {
        std::vector<int> v;
        for (int i = 0; i < 10000; ++i)
            v.push_back(i);
        std::string s = "a string too long to fit inline, so it allocates";
        s += s;
        std::printf("A %zu-int vector and a %zu-byte string,\n"
                    "without calling the allocator even once.\n\n",
                    v.size(), s.size());
    }

    // 2. Calling it explicitly, from C++...
    {
        void *p = util::host_alloc(1024);
        std::printf("C++ : util::host_alloc(1024) -> %p\n", p);
        util::host_free(p);
    }

    // ...and from C, which is the route for third-party libraries.
    {
        void *p = vesta_host_alloc(1024);
        // Rounding up to a size class is not waste if you use it: more than
        // the 1024 you asked for is actually available here.
        std::printf("C   : vesta_host_alloc(1024) -> %p, %zu bytes usable\n", p,
                    vesta_host_usable_size(p));
        vesta_host_free(p);
    }

    // 3. What happened.
    const util::HostAllocStats after = util::host_alloc_stats();
    std::printf("\nsmall allocations : %llu\n",
                (unsigned long long)(after.small_allocs - before.small_allocs));
    std::printf("large allocations : %llu\n",
                (unsigned long long)(after.large_allocs - before.large_allocs));
    std::printf("asked of the OS   : %.1f KiB\n",
                (after.bytes_reserved - before.bytes_reserved) / 1024.0);

    std::printf("\nRun with VESTA_HOST_ALLOC_STATS=1 for a summary at exit,\n"
                "broken down by size and by purpose.\n");
    return 0;
}

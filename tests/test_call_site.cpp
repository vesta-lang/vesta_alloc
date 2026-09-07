/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file tests/test_call_site.cpp
 * @brief That OFF costs nothing, and that ON what it records is TRUE.
 *
 * Four things, each covering a different way for this to go wrong unnoticed:
 *
 *  1. **With it off there is no patch.**  This is the cost guarantee, and the
 *     one that breaks on its own: someone calling `install_call_site_patch()`
 *     too early, or the patch turning back into a permanent thunk, and every
 *     allocation starts paying a jump.  That is not a failure, it is a 3%
 *     nobody attributes to anything.
 *  2. **With it on, the entry is a jump.**  The opposite -- a patch that claims
 *     success and writes nothing -- would give an empty list that looks
 *     perfectly correct.
 *  3. **The address is the REAL caller's**, not a plausible one.  A wrong
 *     address is also different from zero.
 *  4. **`bad_alloc` still propagates** through the patched path.  That is what
 *     validates the unwind directives: if they were wrong this would not turn
 *     the test red, it would take the process down.
 *
 * The "this target cannot do the thunk" case is deliberately not tested here:
 * it is an `#error` in `call_site.cpp`, so it never gets to compile.  An
 * unsupported target does not produce a red test, it produces a stopped build.
 */

#include "util/report/alloc_sites.h"
#include "util/interpose/call_site.h"
#include "util/alloc/host_allocator.h"

#include <cstdint>
#include <cstdio>
#include <new>

namespace {

int failures = 0;

void check(bool ok, const char *what) {
    std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
    if (!ok) ++failures;
}

/// The opcode for `jmp rel32`, which is what the patch writes.
constexpr unsigned char kJmpRel32 = 0xE9;

/* A generous bound for "inside this function".  It need not be exact: what we
 * are ruling out is the recorded address belonging to SOME OTHER site, and a
 * few kilobytes are plenty for that.  Making it exact would need symbols, which
 * is precisely what this test must not depend on. */
constexpr std::size_t kMaxFunctionSize = 4096;

/* An allocation that cannot succeed: 32 TiB, well past what the allocator
 * reserves.
 *
 * `SIZE_MAX/2` would be the natural choice and is NOT used, because of a bug
 * this test found: `chunks_for()` returns `uint32_t`, so at that magnitude the
 * chunk count OVERFLOWS and the allocation "succeeds", handing back a small
 * span -- it neither throws nor returns null.  At 2^45 the count fits and the
 * failure is the one it should be.
 *
 * The bound and the reason are written down here because mixing the two would
 * turn this test red over something that is not its business, and then nobody
 * would look at either. */
constexpr std::size_t kImpossible = std::size_t(1) << 45;

/// The first instruction of a function, to see whether it has been patched.
unsigned char first_byte(void *fn) {
    return *reinterpret_cast<const unsigned char *>(fn);
}

void *plain_new_entry() {
    return reinterpret_cast<void *>(
        static_cast<void *(*)(std::size_t)>(&::operator new));
}

/// The allocation under test comes from here.  Never inlined: if the compiler
/// absorbed it the address would land in the caller and the test would be
/// measuring something else.
[[gnu::noinline]] void *allocate_from_here() {
    return ::operator new(1234);
}

void test_off_costs_nothing() {
    check(!util::call_site_patch_installed(),
          "on start-up, with nothing requested, no patch is installed");
    check(first_byte(plain_new_entry()) != kJmpRel32,
          "`operator new` does NOT begin with a jump: the normal path pays "
          "not one extra instruction");
}

void test_on_the_entry_is_a_jump() {
    check(util::install_call_site_patch(),
          "the patch went into all eight operators");
    check(util::call_site_patch_installed(), "and it is on the record");
    check(first_byte(plain_new_entry()) == kJmpRel32,
          "`operator new` now begins with the jump to the thunk");
}

void test_address_is_the_callers() {
    /* Measurement is switched on by hand rather than through the environment
     * variable: the variable is read BEFORE the first allocation, and by the
     * time a test runs plenty has been allocated already.  The switch is the
     * same one. */
    void *warm_up = ::operator new(16); // give this thread its cache
    ::operator delete(warm_up);
    util::detail::g_measure = true;

    void *p = allocate_from_here();
    util::detail::g_measure = false;
    ::operator delete(p);

    const auto lo = reinterpret_cast<std::uintptr_t>(&allocate_from_here);
    util::AllocSite sites[64];
    const unsigned n = util::alloc_sites_snapshot(sites, 64);
    check(n > 0, "at least one site was recorded");

    bool inside = false;
    for (unsigned i = 0; i < n; ++i) {
        const auto pc = reinterpret_cast<std::uintptr_t>(sites[i].pc);
        if (pc >= lo && pc < lo + kMaxFunctionSize) {
            inside = true;
            break;
        }
    }
    check(inside,
          "the recorded address falls INSIDE the function that allocated, "
          "not somewhere else");
}

void test_bad_alloc_crosses_the_thunk() {
    bool caught = false;
    try {
        /* `volatile` is not decoration: since C++14 the compiler may DELETE an
         * unused `new`/`delete` pair, and then this called nothing and the test
         * failed with nothing actually broken. */
        void *volatile p = ::operator new(kImpossible);
        ::operator delete(const_cast<void *>(p)); // should not be reached
    } catch (const std::bad_alloc &) {
        caught = true;
    }
    check(caught, "`bad_alloc` propagates through the thunk");
}

void test_all_eight_forms() {
    void *a = ::operator new[](64);
    check(a != nullptr, "`operator new[]` hands out memory");
    ::operator delete[](a);

    void *b = ::operator new(64, std::nothrow);
    check(b != nullptr, "the `nothrow` form hands out memory");
    ::operator delete(b, std::nothrow);

    /* And that it does NOT throw when it cannot: that is its whole reason for
     * existing, and it goes through a different thunk (the return address lands
     * in another register). */
    void *volatile c = ::operator new(kImpossible, std::nothrow);
    check(c == nullptr, "the `nothrow` form returns null instead of throwing");

    void *d = ::operator new[](64, std::nothrow);
    check(d != nullptr, "`operator new[]` with `nothrow` hands out memory");
    ::operator delete[](d, std::nothrow);

    /* The OVER-ALIGNED ones.  They go through their own thunks -- the return
     * address lands two registers further along -- and we also have to check
     * they still align: a thunk that got the register wrong could hand back
     * memory anyway, and it would only show up on a misaligned read much
     * later. */
    constexpr std::size_t kAlign = 64;
    void *e = ::operator new(128, std::align_val_t(kAlign));
    check(e != nullptr, "over-aligned `operator new` hands out memory");
    check(reinterpret_cast<std::uintptr_t>(e) % kAlign == 0,
          "and what it hands out really is aligned");
    ::operator delete(e, std::align_val_t(kAlign));

    void *f = ::operator new[](128, std::align_val_t(kAlign));
    check(f != nullptr &&
              reinterpret_cast<std::uintptr_t>(f) % kAlign == 0,
          "over-aligned `operator new[]` hands out aligned memory");
    ::operator delete[](f, std::align_val_t(kAlign));

    void *g = ::operator new(128, std::align_val_t(kAlign), std::nothrow);
    check(g != nullptr &&
              reinterpret_cast<std::uintptr_t>(g) % kAlign == 0,
          "the over-aligned `nothrow` form hands out aligned memory");
    ::operator delete(g, std::align_val_t(kAlign), std::nothrow);

    void *h = ::operator new[](128, std::align_val_t(kAlign), std::nothrow);
    check(h != nullptr &&
              reinterpret_cast<std::uintptr_t>(h) % kAlign == 0,
          "over-aligned `operator new[]` with `nothrow`, the same");
    ::operator delete[](h, std::align_val_t(kAlign), std::nothrow);
}

} // namespace

int main() {
    std::printf("test_call_site: the `operator new` patch\n");
    /* ORDER MATTERS: "off costs nothing" has to be looked at before anything is
     * installed, and once installed there is no going back. */
    test_off_costs_nothing();
    test_on_the_entry_is_a_jump();
    test_address_is_the_callers();
    test_bad_alloc_crosses_the_thunk();
    test_all_eight_forms();
    std::printf("%s (%d failures)\n", failures == 0 ? "ALL OK" : "FAILURES",
                failures);
    return failures == 0 ? 0 : 1;
}

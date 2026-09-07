/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file src/malloc_define.cpp
 * @brief BEING `malloc`, instead of being renamed into it.
 *
 * WHAT THIS BUYS OVER `--wrap`, and it is one thing only, but it is the thing
 * the other way cannot do.  Link-time renaming reaches every call in every
 * object of the link -- and stops there.  What the C library allocates INSIDE
 * itself never becomes a pending reference, so no linker can rename it: that is
 * `strdup`, `getline`, `asprintf`, `realpath` and the rest of the family that
 * hands the caller a block to release.  Today that memory does not appear in
 * the report at all, which reads like "there is none".
 *
 * Defining the symbol closes it.  On ELF a definition in the executable wins
 * over the one in `libc.so`, and the library's own calls go through the PLT --
 * so they land here too.  It is what jemalloc, tcmalloc and mimalloc do, and it
 * is the only mechanism that reaches inside a library nobody recompiled.
 *
 * ELF ONLY, and not for lack of trying: on Windows the C runtime is a DLL and a
 * caller reaches it through the import table, so there is no symbol to outrank.
 * That side is served by `malloc_interpose.cpp` and its `__imp__` pointers.
 *
 * ------------------------------------------------------------------------
 * WHY IT CANNOT SHARE THE ROAD WITH `--wrap`
 * ------------------------------------------------------------------------
 *
 * With `--wrap=malloc` in force AND a definition of `malloc` present, the
 * linker resolves `__real_malloc` against the only definition of `malloc` in
 * the link, which is this one.  The first allocation then calls itself until
 * the stack is gone.  Measured with a three-line probe before any of this was
 * written, because it is the kind of thing that has to be known and not
 * guessed.  So the build removes `--wrap=malloc` and `--wrap=free` when this
 * file is in; see the option in `CMakeLists.txt`.
 *
 * ------------------------------------------------------------------------
 * THE HARD PART: BEING `malloc` BEFORE THE ALLOCATOR EXISTS
 * ------------------------------------------------------------------------
 *
 * The allocator decides whether it is active on its FIRST allocation, and while
 * it is deciding it answers "not active" on purpose -- so that a request made
 * from inside that decision does not re-enter it and hang.  With `--wrap` that
 * gap is harmless: by the time anything in the program calls `malloc`, the C
 * runtime has long since started.  Being `malloc` moves the gap to the worst
 * possible place: the process start-up walks straight through it, and so does
 * the `dlsym` below.
 *
 * And what lives at the bottom of that gap is not a fallback -- it is
 * `no_fallback`, which prints and KILLS the process.  That is the right answer
 * when nobody can allocate except us; it is the wrong answer for the twenty
 * bytes the loader asks for before we are ready.
 *
 * Hence the bootstrap arena: a fixed buffer that serves that window and nothing
 * else.  It never recycles -- start-up allocations live as long as the process
 * anyway -- so it is a bump pointer, and recognising one of its blocks is the
 * same two comparisons that recognise one of the allocator's.  It sits in
 * `.bss`, so it costs address space and not resident memory: a page appears
 * only when it is touched.
 */

/* `RTLD_NEXT` is a GNU extension and has to be asked for before the first
 * header, or `<dlfcn.h>` hides it and the only symptom is a name that does not
 * exist. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "interpose_common.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <dlfcn.h>

namespace {

using vesta_interpose::note_site;
using vesta_interpose::ours;

/**
 * @brief How much the bootstrap arena can serve, in total, ever.
 *
 * Sized for what the C library and the dynamic loader ask for before the
 * allocator has made up its mind, which is a handful of small blocks.  64 KiB
 * is far more than that has ever been measured to need, and it is in `.bss`:
 * the pages that are never touched are never real.  Running out is a loud
 * failure and not a silent one -- see @c boot_alloc.
 */
constexpr size_t kBootBytes = 64u * 1024u;

alignas(util::kAlign) unsigned char g_boot[kBootBytes];
std::atomic<size_t> g_boot_used{0};

/**
 * @brief Each bootstrap block carries its size in front of it.
 *
 * A bump arena does not need this to hand memory out -- but `realloc` does, and
 * one of these blocks can perfectly well reach it: `getline` grows its buffer,
 * and `getline` runs during start-up.  Without the size there is no way to know
 * how much to copy, and guessing means either losing data or reading past the
 * block.  A whole alignment unit so what follows stays aligned to 16.
 */
constexpr size_t kBootHeader = util::kAlign;
static_assert(kBootHeader >= sizeof(size_t),
              "the bootstrap header has to hold a size");

/**
 * @brief Serves out of the bootstrap arena.  Bump pointer, no freeing.
 *
 * Thread safe, and it has to be: nothing says the start-up window belongs to
 * one thread, and a library constructor is free to start one.  The loop is the
 * ordinary compare-and-swap bump -- there is no other state to protect.
 */
void *boot_alloc(size_t n) noexcept {
    if (n == 0) n = 1; // a unique pointer, as the standard asks
    const size_t body = (n + util::kAlign - 1) & ~(size_t)(util::kAlign - 1);
    /* Checked before adding the header, because a size that came from a caller
     * can be large enough to wrap the sum -- and a wrapped sum passes every
     * test that follows it. */
    if (body > kBootBytes || n > (size_t)-1 - util::kAlign) return nullptr;
    const size_t want = body + kBootHeader;

    size_t old = g_boot_used.load(std::memory_order_relaxed);
    size_t next = 0;
    do {
        /* The subtraction and not `old + want > kBootBytes`, for the same
         * reason: this way nothing can wrap. */
        if (want > kBootBytes - old) {
            std::fprintf(stderr,
                         "[allocator] PANIC: the bootstrap arena is full "
                         "(%zu bytes) and %zu more were asked for before the "
                         "allocator was ready. Raise kBootBytes in "
                         "malloc_define.cpp.\n",
                         kBootBytes, want);
            return nullptr;
        }
        next = old + want;
    } while (!g_boot_used.compare_exchange_weak(old, next,
                                                std::memory_order_acq_rel,
                                                std::memory_order_relaxed));
    unsigned char *block = g_boot + old;
    *reinterpret_cast<size_t *>(block) = n;
    return block + kBootHeader;
}

/**
 * @brief The C library's own `free`, for blocks that are not ours.
 *
 * WHY THIS IS NEEDED AT ALL.  The dynamic loader allocates before the program
 * has symbols of its own, and some of what it produces is handed back for the
 * caller to release.  With `--wrap` that block goes to `__real_free`; here
 * there is no `__real_` anything, so the real one has to be looked up.
 *
 * Resolved ONCE and remembered, and resolved LAZILY rather than in a
 * constructor: `dlsym` allocates, and a constructor would run it at a moment we
 * do not control.  Asked for on the first foreign block instead, when the
 * bootstrap arena is already able to answer whatever `dlsym` needs.
 *
 * `RTLD_NEXT` and not `RTLD_DEFAULT`: the default would find OUR definition --
 * we are the first `free` in the search order -- and call it again.
 */
using FreeFn = void (*)(void *);
std::atomic<FreeFn> g_real_free{nullptr};

FreeFn real_free() noexcept {
    FreeFn f = g_real_free.load(std::memory_order_acquire);
    if (f != nullptr) return f;
    f = reinterpret_cast<FreeFn>(::dlsym(RTLD_NEXT, "free"));
    g_real_free.store(f, std::memory_order_release);
    return f;
}

} // namespace

namespace vesta_interpose {

/* The two the OTHER file needs.  Declared in `interpose_common.h`, defined
 * here, and only compiled at all when this file is: with the option off they
 * are an inline `false` and an inline zero, so nothing in the ordinary build
 * carries a branch for an arena that does not exist. */

bool from_bootstrap(const void *p) noexcept {
    const auto v = reinterpret_cast<uintptr_t>(p);
    return v >= reinterpret_cast<uintptr_t>(g_boot) &&
           v < reinterpret_cast<uintptr_t>(g_boot) + kBootBytes;
}

size_t bootstrap_size(const void *p) noexcept {
    return *reinterpret_cast<const size_t *>(
        reinterpret_cast<const unsigned char *>(p) - kBootHeader);
}

} // namespace vesta_interpose

extern "C" {

/**
 * @brief `malloc`, and this time it IS the symbol.
 *
 * The order of the two tests is the whole design.  Asking the allocator whether
 * it is active is what triggers its decision, and during that decision it
 * answers no -- which routes the request to the arena instead of into the
 * re-entry that would hang.  Once it says yes it never goes back, so this costs
 * one predictable branch for the rest of the process.
 */
void *malloc(size_t n) {
    if (__builtin_expect(!util::host_alloc_active(), 0)) return boot_alloc(n);
    void *p = util::host_alloc(n);
    note_site(__builtin_return_address(0), n);
    return p;
}

/**
 * @brief `free`, with three kinds of block to tell apart.
 *
 * Ours is the common case and goes first.  A bootstrap block is DROPPED and
 * that is not a leak being tolerated: the arena does not recycle by design, and
 * what it served is start-up state that lives as long as the process.  Anything
 * else came from the loader, and goes back to the C library that made it.
 *
 * If even that cannot be found, the allocator's own rule applies: a pointer
 * nobody can account for is not an error to swallow.  `no_foreign_free` says so
 * and stops, rather than letting the counts quietly stop adding up.
 */
void free(void *p) {
    if (p == nullptr) return;
    if (__builtin_expect(ours(p), 1)) {
        util::host_free(p);
        return;
    }
    if (vesta_interpose::from_bootstrap(p)) return;
    const FreeFn f = real_free();
    if (f != nullptr) {
        f(p);
        return;
    }
    util::detail::no_foreign_free(p); // does not return
}

} // extern "C"

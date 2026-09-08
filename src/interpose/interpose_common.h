/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file src/interpose_common.h
 * @brief What the two ways of becoming `malloc` both need.
 *
 * THERE ARE TWO WAYS, and which one is in force is decided when the library is
 * built.  They answer different questions, so neither replaces the other:
 *
 *   - `malloc_interpose.cpp` -- renaming at LINK time (`-Wl,--wrap=`).  Reaches
 *     every call in every object of the link and nothing else.  Portable, and
 *     the only one that works on Windows.
 *   - `malloc_define.cpp` -- DEFINING the symbol, the way jemalloc and tcmalloc
 *     do.  On ELF it reaches, on top of that, what the C library allocates
 *     INSIDE itself and hands back (`strdup`, `getline`, `asprintf`), which is
 *     the hole the other one cannot close.  ELF only.
 *
 * They cannot both be in force for the same symbol, and the reason is worth
 * writing down because it is not obvious and it is easy to try: with
 * `--wrap=malloc` AND a definition of `malloc`, the linker resolves
 * `__real_malloc` against the only definition of `malloc` there is -- OURS --
 * and the first allocation recurses until the stack runs out.  Measured, not
 * reasoned: three levels deep and gone.  So the build turns off the renaming
 * for exactly the symbols the other file defines.
 *
 * What lives here is the part that would otherwise be copied into both, and the
 * copy is the dangerous kind: `note_site` decides whether an allocation shows
 * up in the report at all, so two versions of it drifting apart would produce a
 * report that is wrong without looking incomplete.
 */
#ifndef VESTA_ALLOC_INTERPOSE_COMMON_H
#define VESTA_ALLOC_INTERPOSE_COMMON_H

#include "util/report/alloc_sites.h"
#include "util/interpose/call_site.h"
#include "util/alloc/host_allocator.h"
#include "util/alloc/host_allocator_layout.h"
/* Nuestra copia, no la del sistema: en esta libreria la memoria se mueve con
 * nuestras primitivas, y ademas `memcpy` es una de las funciones que un
 * asignador interpuesto no quiere estar llamando desde dentro. */
#include "util/mem/vesta_memcpy.h"
#include "util/mem/vesta_memset.h"

#include <cstddef>

namespace vesta_interpose {

/**
 * @brief Did this block come out of the allocator.
 *
 * Two comparisons, no table and no lock, which is the whole reason freeing is
 * cheap here -- and they are still the only thing the common case runs.
 *
 * WHAT THE TABLE IS FOR.  Everything over `kMaxSpanBytes` is served by the
 * system on a reservation of its own, so it falls outside BOTH regions and the
 * two comparisons say no.  Before, that answer was final and the block went to
 * `no_foreign_free`, which stops the process: `free()` of a 16 MiB `malloc`
 * would have killed any program running with the interposition on.  So when
 * they say no -- and only then, on the branch that was already about to end
 * the program -- the table is asked, and it places the block without reading
 * it, which is what a pointer that might be foreign requires.
 */
[[gnu::always_inline]] inline bool ours(const void *p) noexcept {
    if (__builtin_expect(util::in_region(p) || util::in_big_region(p), 1))
        return true;
    return util::detail::direct_bytes(p) != 0;
}

/**
 * @brief A power of two, and not zero.
 *
 * Every aligned entry checks this first, and not as a formality: the rounding
 * is a mask, so an alignment that is not a power of two would not round -- it
 * would hand back an address that is not aligned and looks like it is.
 */
[[gnu::always_inline]] inline bool pow2(size_t a) noexcept {
    return a != 0 && (a & (a - 1)) == 0;
}

/**
 * @brief Notes where this allocation came from, when sites were asked for.
 *
 * @p ret is the return address of the interposed entry -- the instruction after
 * the `call malloc` in the CALLER.  It is the same thing the `operator new`
 * patch reads off `[rsp]`, so a `malloc` and a `new` from the same function
 * land on one site instead of two.
 *
 * The condition is the same one the C++ path uses, deliberately: measuring
 * (`VESTA_HOST_ALLOC_STATS`) and recording sites (`VESTA_HOST_ALLOC_SITES`) are
 * separate requests, and the patch over `operator new` only goes in for the
 * second.  Recording here on the first alone would fill the table with `malloc`
 * and with nothing from `new` -- a list that looks complete and leaves out half
 * the program.
 *
 * @warning The address is only as good as the call that produced it.  A caller
 * that ends in `return malloc(n);` is compiled as a TAIL CALL by GCC and Clang
 * alike, and then there is no return address of its own to read: what arrives
 * here belongs to ITS caller.  Nothing in this file can tell the difference.
 */
inline void note_site(const void *ret, size_t n) noexcept {
    if (!util::detail::g_measure || !util::call_site_patch_installed()) return;
    const util::detail::ThreadCache *c = util::detail::current_cache();
    /* `have_cache` and not a null test: a thread past its exit notice carries a
     * marker in the slot, and reading `c->tag` off it faults.  The C runtime's
     * own teardown allocates -- that is the whole reason this file exists -- so
     * this is not a corner case, it is every thread.  See
     * `util::detail::kDyingCache`. */
    util::record_alloc_site(ret, n,
                            util::detail::have_cache(c) ? c->tag : 0);
}

/* --------------------------------------------------------------------------
 *  The bootstrap arena, seen from the OTHER side.
 *
 *  Only `malloc_define.cpp` serves out of it, but every path that FREES or
 *  REALLOCATES has to recognise one of its blocks -- handing one to the C
 *  library would be handing it a pointer it never made.  That is why these two
 *  are declared here and not kept private: the arena is not a private detail of
 *  whoever fills it, it is a third kind of block that exists process-wide.
 *
 *  With the option off there is no arena, and these collapse to a constant
 *  `false` and a zero that the optimiser removes outright -- so the ordinary
 *  build does not carry a branch for a thing that cannot happen.
 * ------------------------------------------------------------------------ */
#if defined(VESTA_ALLOC_DEFINE_MALLOC)

/// Whether @p p was served by the bootstrap arena.
bool from_bootstrap(const void *p) noexcept;
/// How many useful bytes that block has.  Only valid for a bootstrap block.
size_t bootstrap_size(const void *p) noexcept;

#else

[[gnu::always_inline]] inline bool from_bootstrap(const void *) noexcept {
    return false;
}
[[gnu::always_inline]] inline size_t bootstrap_size(const void *) noexcept {
    return 0;
}

#endif

} // namespace vesta_interpose

#endif // VESTA_ALLOC_INTERPOSE_COMMON_H

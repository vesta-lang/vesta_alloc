/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file call_site.cpp
 * @brief Implementation of @c util/call_site.h.
 *
 * See that header for why this exists at all.  In short: the return address is
 * taken with one `mov` from `[rsp]` inside a thunk whose prologue we wrote, and
 * the thunk is only reached when measuring, because it is installed by writing
 * a jump over the entry of `operator new` instead of being there all the time.
 *
 * THE OVER-ALIGNED ONES ARE HERE TOO.  They used to record nothing -- they go
 * straight to `host_alloc_aligned` -- so anything aligned to a cache line was
 * invisible to the report.  A missing site leaves no gap behind, so nobody
 * would have noticed.
 */

#include "util/call_site.h"

#include "util/host_allocator.h"
#include "util/os_memory.h"
#include "util/vesta_memcpy.h"

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

/* -------------------------------------------------------------------------
 *  WHAT THIS NEEDS IN ORDER TO WORK
 *
 *  None of it degrades quietly: if the target does not fit, IT DOES NOT
 *  COMPILE.  A silent fallback would hand the value back to the compiler right
 *  after we took it away, and that is not something you notice by looking --
 *  the report would come out plausible and wrong.
 * ------------------------------------------------------------------------- */

/* LTO BREAKS THIS, and the check is NOT here.
 *
 * The patch assumes every allocation goes through the ENTRY of `operator new`.
 * That holds always -- it is replaceable and lives in another translation unit
 * -- except under link-time optimisation, which may inline it into its callers:
 * those allocations no longer pass through the patched entry and stop being
 * recorded.  Without failing and without warning.
 *
 * It cannot be stopped from here because the preprocessor does not know:
 * neither GCC nor Clang define any macro for `-flto` (checked with `-dM -E`).
 * Whoever configures the build does know, so the `FATAL_ERROR` lives in this
 * library's `CMakeLists.txt`.  It is said here so that anyone reading the
 * mechanism learns about the condition holding it up. */

#if !defined(__x86_64__) && !defined(_M_X64)
#error "call_site.cpp: thunks only exist for x86-64.  Adding a target needs \
four things: where the calling convention leaves the return address, which \
register each integer argument goes in, how a relative jump is encoded for the \
patch, and the format's unwind directives.  Until they are there, not \
compiling beats recording made-up addresses."
#endif

static_assert(sizeof(void *) == 8,
              "call_site.cpp: the thunks read an 8-byte pointer from [rsp].");

namespace {

/// `jmp rel32`: one opcode plus a signed displacement relative to the END of
/// the instruction.  The shortest encoding that still reaches the whole module.
constexpr std::size_t kJumpBytes = 5;

} // namespace

extern "C" {

/**
 * @brief The real `operator new(size_t)`, with the return address in hand.
 *
 * May throw `std::bad_alloc`, just like the operator it stands in for.  Having
 * C linkage does not prevent that: linkage fixes the name, not what the
 * function is allowed to do.  It lives in `host_allocator.cpp`, next to the
 * cold path it uses.
 */
void *vesta_alloc_new_from(std::size_t n, const void *ret);

/// The non-throwing form; returns null when there is no memory.
void *vesta_alloc_new_nothrow_from(std::size_t n, const std::nothrow_t &,
                                   const void *ret) noexcept;

/// The OVER-ALIGNED forms.  They go in the same way as the rest: everything
/// that allocates has to be able to say where it came from, or the list of
/// sites lies by omission.
void *vesta_alloc_new_aligned_from(std::size_t n, std::size_t a,
                                   const void *ret);
void *vesta_alloc_new_aligned_nothrow_from(std::size_t n, std::size_t a,
                                           const std::nothrow_t &,
                                           const void *ret) noexcept;

/* The thunks.  These are not `operator new`: they are where the patch points.
 *
 * The return address goes into the first argument register the signature leaves
 * free:
 *
 *   signature                            Win64             System V
 *   (size_t)                             rcx | rdx         rdi | rsi
 *   (size_t, const nothrow_t &)          rcx,rdx | r8      rdi,rsi | rdx
 *   (size_t, align_val_t)                rcx,rdx | r8      rdi,rsi | rdx
 *   (size_t, align_val_t, nothrow_t &)   rcx,rdx,r8 | r9   rdi,rsi,rdx | rcx
 *
 * `align_val_t` is an `enum class` whose underlying type is `size_t`, so it
 * travels in an integer register like any other argument: the two aligned forms
 * land in the same slot as their unaligned counterparts.
 *
 * Entry is by `jmp`, not `call`, so `[rsp]` still holds the REAL caller's
 * return address -- which is exactly what we want -- and the unwinder sees the
 * same frame it would see without any of this, so `bad_alloc` propagates as
 * before. */
void vesta_alloc_thunk_new(void);
void vesta_alloc_thunk_new_nothrow(void);
void vesta_alloc_thunk_new_aligned(void);
void vesta_alloc_thunk_new_aligned_nothrow(void);

} // extern "C"

#if defined(_WIN32)

/* INTEL SYNTAX (the NASM one), not AT&T.  It is what the rest of the project
 * uses to write assembly, and it matters more than usual here: these eight
 * instructions are the whole mechanism, and they get read far more often than
 * they get written.
 *
 * `noprefix` so registers need no decoration, and the syntax is RESTORED on the
 * way out: the assembler tracks it per file, and leaving it switched breaks
 * what GCC emits afterwards -- a failure that shows up in some other function,
 * not here.
 *
 * `.seh_proc` with an EMPTY prologue.  The thunk never touches the stack, so no
 * return address should ever point inside it; the directives are there anyway,
 * because an executable region with no unwind information on Win64 is what
 * turns an exception into a crash.
 *
 * Each directive on its own line: GAS accepts `;` as a separator, but a
 * semicolon inside a C string confuses syntax highlighting and the file reads
 * wrong from there on.  Writing it properly costs the same. */
asm(".intel_syntax noprefix\n"
    ".text\n"
    ".globl vesta_alloc_thunk_new\n"
    ".def vesta_alloc_thunk_new\n"
    ".scl 2\n"
    ".type 32\n"
    ".endef\n"
    ".seh_proc vesta_alloc_thunk_new\n"
    "vesta_alloc_thunk_new:\n"
    "  .seh_endprologue\n"
    "  mov rdx, [rsp]\n"
    "  jmp vesta_alloc_new_from\n"
    "  .seh_endproc\n"
    ".globl vesta_alloc_thunk_new_nothrow\n"
    ".def vesta_alloc_thunk_new_nothrow\n"
    ".scl 2\n"
    ".type 32\n"
    ".endef\n"
    ".seh_proc vesta_alloc_thunk_new_nothrow\n"
    "vesta_alloc_thunk_new_nothrow:\n"
    "  .seh_endprologue\n"
    "  mov r8, [rsp]\n"
    "  jmp vesta_alloc_new_nothrow_from\n"
    "  .seh_endproc\n"
    ".globl vesta_alloc_thunk_new_aligned\n"
    ".def vesta_alloc_thunk_new_aligned\n"
    ".scl 2\n"
    ".type 32\n"
    ".endef\n"
    ".seh_proc vesta_alloc_thunk_new_aligned\n"
    "vesta_alloc_thunk_new_aligned:\n"
    "  .seh_endprologue\n"
    "  mov r8, [rsp]\n"
    "  jmp vesta_alloc_new_aligned_from\n"
    "  .seh_endproc\n"
    ".globl vesta_alloc_thunk_new_aligned_nothrow\n"
    ".def vesta_alloc_thunk_new_aligned_nothrow\n"
    ".scl 2\n"
    ".type 32\n"
    ".endef\n"
    ".seh_proc vesta_alloc_thunk_new_aligned_nothrow\n"
    "vesta_alloc_thunk_new_aligned_nothrow:\n"
    "  .seh_endprologue\n"
    "  mov r9, [rsp]\n"
    "  jmp vesta_alloc_new_aligned_nothrow_from\n"
    "  .seh_endproc\n"
    ".att_syntax prefix\n");

#else

/* Same as the Windows block; see it for the reasoning.  `@PLT` because in a
 * position-independent build the target is reached through the table; the
 * linker relaxes it to a direct jump when that is not needed, so it costs
 * nothing where it is not required. */
asm(".intel_syntax noprefix\n"
    ".text\n"
    ".globl vesta_alloc_thunk_new\n"
    ".type vesta_alloc_thunk_new, @function\n"
    "vesta_alloc_thunk_new:\n"
    "  .cfi_startproc\n"
    "  mov rsi, [rsp]\n"
    "  jmp vesta_alloc_new_from@PLT\n"
    "  .cfi_endproc\n"
    ".size vesta_alloc_thunk_new, .-vesta_alloc_thunk_new\n"
    ".globl vesta_alloc_thunk_new_nothrow\n"
    ".type vesta_alloc_thunk_new_nothrow, @function\n"
    "vesta_alloc_thunk_new_nothrow:\n"
    "  .cfi_startproc\n"
    "  mov rdx, [rsp]\n"
    "  jmp vesta_alloc_new_nothrow_from@PLT\n"
    "  .cfi_endproc\n"
    ".size vesta_alloc_thunk_new_nothrow, .-vesta_alloc_thunk_new_nothrow\n"
    ".globl vesta_alloc_thunk_new_aligned\n"
    ".type vesta_alloc_thunk_new_aligned, @function\n"
    "vesta_alloc_thunk_new_aligned:\n"
    "  .cfi_startproc\n"
    "  mov rdx, [rsp]\n"
    "  jmp vesta_alloc_new_aligned_from@PLT\n"
    "  .cfi_endproc\n"
    ".size vesta_alloc_thunk_new_aligned, .-vesta_alloc_thunk_new_aligned\n"
    ".globl vesta_alloc_thunk_new_aligned_nothrow\n"
    ".type vesta_alloc_thunk_new_aligned_nothrow, @function\n"
    "vesta_alloc_thunk_new_aligned_nothrow:\n"
    "  .cfi_startproc\n"
    "  mov rcx, [rsp]\n"
    "  jmp vesta_alloc_new_aligned_nothrow_from@PLT\n"
    "  .cfi_endproc\n"
    ".size vesta_alloc_thunk_new_aligned_nothrow, "
    ".-vesta_alloc_thunk_new_aligned_nothrow\n"
    ".att_syntax prefix\n");

#endif

namespace util {
namespace {

/**
 * @brief Writes `jmp target` over the first bytes of @p entry.
 *
 * @return false if the system will not let us write there, or if the target
 *         falls outside the reach of a 32-bit relative jump.  Both cases are
 *         reported: whoever patches has to be able to SAY it could not.
 */
bool write_jump(void *entry, const void *target) noexcept {
    unsigned char *p = static_cast<unsigned char *>(entry);
    const std::int64_t rel = std::int64_t(std::uintptr_t(target)) -
                             std::int64_t(std::uintptr_t(p) + kJumpBytes);
    if (rel < INT32_MIN || rel > INT32_MAX) return false;

    if (!os_protect(p, kJumpBytes, OsProt::Read | OsProt::Write | OsProt::Exec))
        return false;
    const std::int32_t rel32 = std::int32_t(rel);
    p[0] = 0xE9;
    /* OUR `memcpy`, not the library one.  It is four bytes, but that is not the
     * point: this library ships its own, and reaching outside for it here would
     * add a dependency we do not need, in the only place in the file that
     * writes memory. */
    vesta_memcpy(p + 1, &rel32, sizeof(rel32));
    if (!os_protect(p, kJumpBytes, OsProt::Read | OsProt::Exec)) {
        /* Written, but write permission could not be taken back.  The jump is
         * good, so measurement works; what is left is a page more permissive
         * than it should be.  Still returns true -- pretending it failed would
         * leave the code patched and the caller believing it is not. */
    }
    return true;
}

bool g_patched = false;

} // namespace

bool install_call_site_patch() noexcept {
    if (g_patched) return true; // once is enough
    g_patched = true;

    struct Patch {
        void *entry;
        const void *thunk;
    };
    /* All EIGHT symbols, in four pairs: `new` and `new[]` do the same thing, so
     * they share a destination. */
    const Patch patches[8] = {
        {reinterpret_cast<void *>(
             static_cast<void *(*)(std::size_t)>(&::operator new)),
         reinterpret_cast<const void *>(&vesta_alloc_thunk_new)},
        {reinterpret_cast<void *>(
             static_cast<void *(*)(std::size_t)>(&::operator new[])),
         reinterpret_cast<const void *>(&vesta_alloc_thunk_new)},
        {reinterpret_cast<void *>(
             static_cast<void *(*)(std::size_t, const std::nothrow_t &)>(
                 &::operator new)),
         reinterpret_cast<const void *>(&vesta_alloc_thunk_new_nothrow)},
        {reinterpret_cast<void *>(
             static_cast<void *(*)(std::size_t, const std::nothrow_t &)>(
                 &::operator new[])),
         reinterpret_cast<const void *>(&vesta_alloc_thunk_new_nothrow)},
        // And the over-aligned ones, which used to record nothing.
        {reinterpret_cast<void *>(
             static_cast<void *(*)(std::size_t, std::align_val_t)>(
                 &::operator new)),
         reinterpret_cast<const void *>(&vesta_alloc_thunk_new_aligned)},
        {reinterpret_cast<void *>(
             static_cast<void *(*)(std::size_t, std::align_val_t)>(
                 &::operator new[])),
         reinterpret_cast<const void *>(&vesta_alloc_thunk_new_aligned)},
        {reinterpret_cast<void *>(
             static_cast<void *(*)(std::size_t, std::align_val_t,
                                   const std::nothrow_t &)>(&::operator new)),
         reinterpret_cast<const void *>(
             &vesta_alloc_thunk_new_aligned_nothrow)},
        {reinterpret_cast<void *>(
             static_cast<void *(*)(std::size_t, std::align_val_t,
                                   const std::nothrow_t &)>(&::operator new[])),
         reinterpret_cast<const void *>(
             &vesta_alloc_thunk_new_aligned_nothrow)},
    };

    bool all_ok = true;
    for (const Patch &patch : patches)
        if (!write_jump(patch.entry, patch.thunk)) all_ok = false;
    return all_ok;
}

bool call_site_patch_installed() noexcept { return g_patched; }

} // namespace util

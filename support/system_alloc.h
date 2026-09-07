/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file support/system_alloc.h
 * @brief The system allocator, still reachable AFTER this library has replaced
 *        it, so a benchmark can put the two side by side in one process.
 *
 * THE PROBLEM.  Comparing against the system allocator used to mean running the
 * binary twice, once with `VESTA_NO_HOST_SLAB=1`.  Two processes cannot be
 * interleaved, and without interleaving the two columns are measured at
 * different seconds of clock: if the machine slows down for one of them, the
 * comparison silently absorbs it.  That is the single biggest reason a
 * benchmark reports a difference that is not there.
 *
 * And in this process there is nothing left to compare against.  On Windows the
 * library writes a jump over the entry of `msvcrt!malloc` and keeps NO
 * trampoline -- deliberately, because a trampoline is a second door into a heap
 * that must have only one.  `__real_malloc` does not help either: it is the
 * `--wrap` original, which is that same patched entry.
 *
 * THE WAY OUT is that the image in memory need not be the only image.  A DLL
 * loaded from a DIFFERENT PATH is mapped as a separate module with its own
 * copy of the code, and the patch never touched that one.  Copy the C runtime
 * to a scratch file, load THAT, and its `malloc` is the genuine article.
 *
 * Measured, three ways, before this was built on:
 *
 * @code
 * msvcrt already loaded at     00007ffe53b20000
 * our private copy at          00007ffe11c00000   <- a second image
 * patched malloc entry         ff 25 00 00 00 00  <- jmp [rip+0], ours
 * private copy malloc entry    48 89 5c 24 08     <- a real prologue
 * control, our own 64 bytes    recognised as ours
 * 64 bytes from private malloc no -- genuine system memory
 * 64 bytes from patched malloc ours -- the patch IS installed
 * @endcode
 *
 * The last two lines are the pair that matters: without the second one, "the
 * copy escaped the patch" could not be told apart from "the patch was never
 * installed".
 *
 * THE ONE RULE FOR CALLERS: a block from this API is freed with THIS API.  The
 * private copy has its own heap, and handing one of its blocks to any other
 * `free` corrupts both.  That is why the two function pointers travel together
 * in one struct instead of being fetched separately.
 *
 * @code
 * const system_alloc::Api &sys = system_alloc::api();
 * if (!sys.ok()) {
 *     std::printf("no system column: %s\n", sys.why);
 * } else {
 *     void *p = sys.alloc(64);
 *     sys.release(p);              // never plain free(p)
 * }
 * @endcode
 */

#ifndef VESTA_SUPPORT_SYSTEM_ALLOC_H
#define VESTA_SUPPORT_SYSTEM_ALLOC_H

#include <cstddef>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace system_alloc {

/// The genuine `malloc`/`calloc`/`realloc`/`free`, plus what had to be done to
/// reach them.
struct Api {
    void *(*alloc)(size_t) = nullptr;
    void (*release)(void *) = nullptr;
    /* THE OTHER TWO, and they are not decoration.  A benchmark that compares
     * only `malloc` leaves out the two calls where the allocators differ MOST:
     * `calloc` because whether the zeros are written or come free from the
     * kernel is a design decision each side makes differently, and `realloc`
     * because growing in place or copying is the whole question.  Reaching them
     * through the same handle as `malloc` is also what keeps a block with the
     * pair that made it -- see the rule below. */
    void *(*zeroed)(size_t, size_t) = nullptr;
    void *(*grow)(void *, size_t) = nullptr;
    /// How it was reached, for the report to print.  Never a guess: it says
    /// which of the routes below actually worked.
    const char *how = "";
    /// Why it could NOT be reached, when it could not.  A benchmark that drops
    /// a column in silence looks like a benchmark that measured no difference.
    const char *why = "";

    bool ok() const {
        return alloc != nullptr && release != nullptr && zeroed != nullptr &&
               grow != nullptr;
    }
};

namespace detail {

#if defined(_WIN32)

/**
 * @brief Loads a private copy of the C runtime and takes its allocator.
 *
 * The copy goes to the temp directory under a fixed name.  Fixed and not
 * unique on purpose: a unique name would leave one file per run, and the file
 * cannot be deleted while it is loaded -- the process holds it until it exits.
 * With a fixed name the second run overwrites the first, or fails to and loads
 * the copy that is already there, which is the same DLL either way.
 */
inline Api load_private_crt() {
    Api a;

    char sysdir[MAX_PATH];
    if (GetSystemDirectoryA(sysdir, MAX_PATH) == 0) {
        a.why = "could not find the system directory";
        return a;
    }
    char tmpdir[MAX_PATH];
    if (GetTempPathA(MAX_PATH, tmpdir) == 0) {
        a.why = "could not find the temp directory";
        return a;
    }

    const std::string src = std::string(sysdir) + "\\msvcrt.dll";
    const std::string dst = std::string(tmpdir) + "vesta_bench_crt.dll";

    /* A failure to copy is NOT fatal by itself: the file may already be there
     * and locked by another run of this benchmark, and that copy is just as
     * good.  What decides is whether it loads. */
    CopyFileA(src.c_str(), dst.c_str(), FALSE);

    HMODULE priv = LoadLibraryA(dst.c_str());
    if (priv == nullptr) {
        a.why = "could not load a private copy of msvcrt.dll";
        return a;
    }

    /* And it must be a DIFFERENT module from the one already loaded.  If the
     * loader handed back the same one -- same path resolved, or the copy failed
     * and an old identical file matched -- then its `malloc` carries our jump
     * and the "system" column would silently be us. */
    if (priv == GetModuleHandleA("msvcrt.dll")) {
        a.why = "the loader returned the msvcrt already in this process";
        return a;
    }

    a.alloc = reinterpret_cast<void *(*)(size_t)>(
        reinterpret_cast<void *>(GetProcAddress(priv, "malloc")));
    a.release = reinterpret_cast<void (*)(void *)>(
        reinterpret_cast<void *>(GetProcAddress(priv, "free")));
    a.zeroed = reinterpret_cast<void *(*)(size_t, size_t)>(
        reinterpret_cast<void *>(GetProcAddress(priv, "calloc")));
    a.grow = reinterpret_cast<void *(*)(void *, size_t)>(
        reinterpret_cast<void *>(GetProcAddress(priv, "realloc")));
    if (!a.ok()) {
        a.alloc = nullptr;
        a.release = nullptr;
        a.zeroed = nullptr;
        a.grow = nullptr;
        a.why = "the private copy is missing one of malloc/calloc/realloc/free";
        return a;
    }
    a.how = "a private copy of msvcrt.dll, loaded from disk";
    return a;
}

#else

/**
 * @brief Takes libc's own allocator by handle.
 *
 * Simpler than the Windows side, and for a different reason: here the library
 * takes over by DEFINING the symbols, so nothing is patched.  Asking a specific
 * library handle for its `malloc` gives that library's implementation, which is
 * the one interposition routes around rather than the one it replaced.
 */
inline Api load_libc() {
    Api a;
    void *h = dlopen("libc.so.6", RTLD_LAZY | RTLD_LOCAL);
    const char *how = "libc.so.6, by handle";
    if (h == nullptr) {
        h = dlopen("libc.so", RTLD_LAZY | RTLD_LOCAL);
        how = "libc.so, by handle";
    }
    if (h == nullptr) {
        a.why = "could not open libc";
        return a;
    }
    a.alloc = reinterpret_cast<void *(*)(size_t)>(dlsym(h, "malloc"));
    a.release = reinterpret_cast<void (*)(void *)>(dlsym(h, "free"));
    a.zeroed = reinterpret_cast<void *(*)(size_t, size_t)>(dlsym(h, "calloc"));
    a.grow = reinterpret_cast<void *(*)(void *, size_t)>(dlsym(h, "realloc"));
    if (!a.ok()) {
        a.alloc = nullptr;
        a.release = nullptr;
        a.zeroed = nullptr;
        a.grow = nullptr;
        a.why = "libc is missing one of malloc/calloc/realloc/free";
        return a;
    }
    a.how = how;
    return a;
}

#endif

} // namespace detail

/**
 * @brief The system allocator, resolved once.
 *
 * @return An @c Api whose @c ok() says whether there is a system column at all.
 */
inline const Api &api() {
#if defined(_WIN32)
    static const Api a = detail::load_private_crt();
#else
    static const Api a = detail::load_libc();
#endif
    return a;
}

} // namespace system_alloc

#endif // VESTA_SUPPORT_SYSTEM_ALLOC_H

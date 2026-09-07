/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file util/os_env.cpp
 * @brief Reading the environment block the system gave us.  Reasons: header.
 *
 * System headers ARE allowed in here: this is a single translation unit and
 * what they define -- `VOID` among other things -- does not leak out of it.
 *
 * RULE OF THIS FILE, the same as `os_memory.cpp`: **no function allocates,
 * prints or throws.**  It also may not ask the C runtime anything about the
 * environment, which is the whole point of the file.
 */

#include "util/os_env.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

/* The pieces of the PEB that lead to the environment.  Written out here rather
 * than taken from `<winternl.h>`, for the same reason `os_memory.cpp` writes
 * its own: that header declares these structures with the interesting fields
 * replaced by `Reserved`, and `Environment` is one of the replaced ones.
 *
 * The reserved gaps are kept EXACTLY as documented.  They are what puts the
 * fields after them at the right offset, so shortening one would not fail to
 * compile -- it would read some other pointer and then walk it. */

/// A counted UTF-16 string, as the kernel stores them.
struct VestaUnicodeString {
    USHORT Length;
    USHORT MaximumLength;
    wchar_t *Buffer;
};

/// `RTL_USER_PROCESS_PARAMETERS`, up to the field we came for.
struct VestaProcessParameters {
    BYTE Reserved1[16];
    PVOID Reserved2[10];
    VestaUnicodeString ImagePathName;
    VestaUnicodeString CommandLine;
    /// `NAME=VALUE\0` one after another, closed by an empty string.
    wchar_t *Environment;
};

/// The `PEB`, up to the pointer to the process parameters.
struct VestaPeb {
    BYTE Reserved1[2];
    BYTE BeingDebugged;
    BYTE Reserved2[1];
    PVOID Reserved3[2];
    PVOID Ldr;
    VestaProcessParameters *ProcessParameters;
};

/// What class 0 of `NtQueryInformationProcess` returns.
struct VestaProcessBasicInformation {
    PVOID Reserved1;
    VestaPeb *PebBaseAddress;
    PVOID Reserved2[2];
    ULONG_PTR UniqueProcessId;
    PVOID Reserved3;
};

/* Declared here and not pulled from `<winternl.h>`, same as in
 * `os_memory.cpp`: that header declares half of these and drags in types that
 * are not needed.  It is LINKED rather than resolved at run time, so a missing
 * symbol -- which would mean a broken system, not an old one -- stops the
 * process at load instead of failing later and somewhere else. */
extern "C" LONG __stdcall NtQueryInformationProcess(
    HANDLE ProcessHandle, ULONG ProcessInformationClass,
    PVOID ProcessInformation, ULONG ProcessInformationLength,
    PULONG ReturnLength);

/// The current process.  A constant pseudo-handle; there is nothing to close.
#define VESTA_NT_SELF (reinterpret_cast<HANDLE>(static_cast<intptr_t>(-1)))

#elif defined(__APPLE__)
/* macOS does not export `environ` to anything but the main executable, so the
 * accessor is the only form that also works from inside a shared library. */
#include <crt_externs.h>
#else
/* Set by the loader, pointing into the initial stack, before any constructor
 * runs.  It is a variable, not a call: nothing has to be built first, which is
 * exactly what `getenv` cannot promise. */
extern "C" char **environ;
#endif

namespace util {

namespace {

#if defined(_WIN32)

/**
 * @brief ASCII lower-case fold, without `<cctype>`.
 *
 * `tolower` depends on the current locale, and the locale is one more thing the
 * C runtime builds while starting up -- precisely what this file cannot wait
 * for.  Variable names are ASCII, so the fold is a subtraction.
 *
 * Only Windows needs it: there variable names are case-insensitive.  On POSIX
 * they are not, so the comparison is exact and this would sit unused -- which
 * clang says out loud, and it is right.
 */
inline char fold(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
}

/**
 * @brief The process environment block, asked of the kernel.
 * @return nullptr if the system will not say -- a broken process rather than an
 *         old one.  Every switch then reads as off, which is the same answer as
 *         nobody having set one.
 *
 * Asked on every call instead of cached.  It runs a handful of times during
 * start-up, and a cache here would be a static with a guard: a guard below the
 * allocator is what already cost one hang (see `util/thread_slot.h`).
 */
const wchar_t *environment_block() noexcept {
    VestaProcessBasicInformation pbi;
    pbi.PebBaseAddress = nullptr;
    /* Class 0 is `ProcessBasicInformation`.  A negative status is a failure;
     * anything else means the structure was filled in. */
    if (NtQueryInformationProcess(VESTA_NT_SELF, 0, &pbi, sizeof(pbi),
                                  nullptr) < 0)
        return nullptr;
    if (pbi.PebBaseAddress == nullptr) return nullptr;
    const VestaProcessParameters *pp = pbi.PebBaseAddress->ProcessParameters;
    if (pp == nullptr) return nullptr;
    return pp->Environment;
}

#else

/// The environment as the loader left it.
inline char **environment_block() noexcept {
#if defined(__APPLE__)
    char ***p = _NSGetEnviron();
    return (p != nullptr) ? *p : nullptr;
#else
    return environ;
#endif
}

#endif // _WIN32

/// Terminates @p buf for a value of @p n characters, truncating if it must.
inline void terminate(char *buf, size_t cap, size_t n) noexcept {
    if (cap == 0) return;
    buf[(n + 1 < cap) ? n : (cap - 1)] = '\0';
}

} // namespace

size_t os_env(const char *name, char *buf, size_t cap) noexcept {
    if (cap != 0) buf[0] = '\0';
    if (name == nullptr || name[0] == '\0') return kOsEnvUnset;

#if defined(_WIN32)
    const wchar_t *block = environment_block();
    if (block == nullptr) return kOsEnvUnset;

    /* Entries run `NAME=VALUE\0` one after another and the block is closed by
     * an empty entry, so a leading nul means there is nothing left. */
    for (const wchar_t *p = block; *p != L'\0';) {
        /* Names are matched case-insensitively because that is what Windows
         * does -- `Path` and `PATH` are one variable there -- and only up to
         * the `=`.  An entry whose first character IS `=` (the shell hides
         * per-drive directories that way) can never match, which is right. */
        size_t i = 0;
        while (p[i] < 0x80 && p[i] != L'\0' && p[i] != L'=' &&
               name[i] != '\0' && fold(char(p[i])) == fold(name[i]))
            ++i;
        const bool hit = p[i] == L'=' && name[i] == '\0';

        /* Walk to the end of this entry either way: the next one begins one
         * past its terminator. */
        const wchar_t *end = p + i;
        while (*end != L'\0') ++end;
        if (!hit) {
            p = end + 1;
            continue;
        }

        size_t n = 0;
        for (const wchar_t *q = p + i + 1; q != end; ++q, ++n) {
            /* UTF-16 narrowed to ASCII.  Anything else becomes `?` and not the
             * low byte of the code unit, which could land on `0` or `1` and
             * turn a name we cannot read into an answer. */
            if (n + 1 < cap) buf[n] = (*q < 0x80) ? char(*q) : '?';
        }
        terminate(buf, cap, n);
        return n;
    }
    return kOsEnvUnset;
#else
    char **block = environment_block();
    if (block == nullptr) return kOsEnvUnset;

    for (char **e = block; *e != nullptr; ++e) {
        const char *p = *e;
        /* Same shape as the Windows branch on purpose.  The one difference is
         * that POSIX names are case-SENSITIVE, and that is the system's rule,
         * not a choice made here. */
        size_t i = 0;
        while (p[i] != '\0' && p[i] != '=' && name[i] != '\0' &&
               p[i] == name[i])
            ++i;
        if (p[i] != '=' || name[i] != '\0') continue;

        size_t n = 0;
        for (const char *q = p + i + 1; *q != '\0'; ++q, ++n)
            if (n + 1 < cap) buf[n] = *q;
        terminate(buf, cap, n);
        return n;
    }
    return kOsEnvUnset;
#endif
}

bool os_env_flag(const char *name) noexcept {
    /* Four characters are more than enough to tell the three answers apart:
     * unset, `"0"`, and anything else.  A longer value is truncated and still
     * reads as on, which is the right answer for everything that is not `"0"`.
     * The buffer is on the stack because there is nowhere else to put it. */
    char v[4];
    const size_t n = os_env(name, v, sizeof(v));
    if (n == kOsEnvUnset || n == 0) return false;
    return !(n == 1 && v[0] == '0');
}

} // namespace util

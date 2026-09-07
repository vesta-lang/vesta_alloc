/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file util/os_env.h
 * @brief Reading the environment from the SYSTEM, not from the C runtime.
 *
 * WHY THIS EXISTS, WHEN `getenv` IS ONE LINE.  Because `getenv` does not read
 * the environment: it reads a COPY that the C runtime builds while it starts
 * up, and this library runs before that copy exists.
 *
 * The allocator initialises on the first allocation, and the first allocation
 * can happen during static initialisation -- any global whose constructor asks
 * for memory gets there before `main`.  Whether the C runtime's environment
 * table is built by then is not something this library gets to decide: it
 * depends on the runtime, and on link order.  When it is not, `getenv` returns
 * null, every switch reads as off, and nothing fails and nothing is printed --
 * measurement simply does not happen.
 *
 * That is the worst shape a bug can have here.  It is invisible, it depends on
 * which global happened to be constructed first, and it would come and go as
 * unrelated code is added.  So rather than establish whether today's runtime
 * and today's link order happen to be safe, the question is asked of something
 * that is ALWAYS ready.
 *
 * WHAT THIS DOES INSTEAD.  It goes to where the environment actually lives, the
 * block the kernel handed the process:
 *
 *   Windows  the PEB, reached with `NtQueryInformationProcess`.  The kernel
 *            fills it in before the first instruction of the process runs, so
 *            there is no "too early".
 *   POSIX    `environ`, which the loader points at the initial stack before any
 *            constructor runs.
 *
 * It also means this library does not need the C runtime to answer a question
 * about the process -- the same reason `util/os_memory.h` talks to ntdll rather
 * than to kernel32.
 *
 * RULE OF THIS MODULE, the same as `os_memory`: **nothing here allocates,
 * prints or throws.**  It sits below the allocator, so there is nothing to fall
 * back to; a failure is a return value.
 */
#ifndef VESTA_UTIL_OS_ENV_H
#define VESTA_UTIL_OS_ENV_H

#include <cstddef>

namespace util {

/// Returned by @c os_env when the variable is not in the environment at all.
/// Distinct from a variable that IS set and empty, which returns 0 -- the two
/// mean different things to whoever asked, and merging them hides one of them.
inline constexpr size_t kOsEnvUnset = static_cast<size_t>(-1);

/**
 * @brief Copies the value of an environment variable into a caller's buffer.
 * @param name  Variable name, ASCII.  Case-insensitive on Windows, exact on
 *              POSIX -- that is the system's rule, not ours.
 * @param buf   Where the value goes.  Always nul-terminated when @p cap > 0.
 * @param cap   Size of @p buf in bytes.
 * @return The FULL length of the value, or @c kOsEnvUnset if it is not set.
 *
 * The return value is the length the value HAS, not the length that was
 * written: a result >= @p cap means it did not fit and what is in @p buf is
 * truncated.  That is on purpose -- returning the truncated length would hand
 * back a short value that looks complete, which is the kind of quiet wrong
 * answer this project does not accept.
 *
 * The buffer is the caller's because there is nowhere to put one: allocating
 * here would call the allocator, and the allocator is what calls this.
 *
 * @note On Windows the environment is UTF-16.  Values are narrowed byte by
 *       byte and anything outside ASCII becomes `?`.  What this library asks
 *       about are switches, so a value that needs more than ASCII is already
 *       not an answer to the question -- but it is visible rather than silently
 *       mangled into something that could parse.
 *
 * @par Threads
 * Safe.  Reads a block the process does not modify through this path; it does
 * NOT see a later `setenv`/`SetEnvironmentVariable`, which is what makes it
 * usable before the C runtime exists.
 *
 * @code
 *   char buf[32];
 *   const size_t n = util::os_env("VESTA_HOST_ALLOC_TAG", buf, sizeof(buf));
 *   if (n == util::kOsEnvUnset) return kDefaultTag;   // nobody asked
 *   if (n >= sizeof(buf)) return kDefaultTag;         // asked for nonsense
 * @endcode
 */
size_t os_env(const char *name, char *buf, size_t cap) noexcept;

/**
 * @brief The question this library actually asks: is this switch on?
 * @return true when the variable is set, non-empty, and not exactly `"0"`.
 *
 * One place decides what "on" means, so `VESTA_HOST_ALLOC_STATS=0` cannot mean
 * one thing here and the opposite three files away.
 *
 * @par Threads
 * Safe.  Allocates nothing.
 *
 * @code
 *   // Read ONCE, before the first allocation: switching measurement on later
 *   // would leave everything from start-up out without saying so.
 *   const bool sites = util::os_env_flag("VESTA_HOST_ALLOC_SITES");
 * @endcode
 */
bool os_env_flag(const char *name) noexcept;

} // namespace util

#endif // VESTA_UTIL_OS_ENV_H

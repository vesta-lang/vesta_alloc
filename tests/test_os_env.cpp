/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file tests/util/test_os_env.cpp
 * @brief That reading the environment from the SYSTEM says the same as `getenv`.
 *
 * WHAT MUST NOT BREAK.  `util::os_env` exists because `getenv` cannot be asked
 * early enough -- it reads a copy the C runtime builds while starting up, and
 * the allocator can run before that copy exists.  But "earlier" is only worth
 * anything if the ANSWER is the same one, so the core of this test is a
 * comparison against `getenv` over the whole environment: every variable the C
 * runtime can see, this must see too, with the same value.
 *
 * That comparison is what would catch the mistakes this code can actually make
 * -- an entry skipped because the walk landed one character off, a name matched
 * against the wrong half of `NAME=VALUE`, a value truncated without saying so.
 * Testing a couple of variables we set ourselves would pass through all three.
 *
 * `main` takes `envp` on purpose: it is a THIRD source, independent of both,
 * and it is what proves the count is right rather than merely self-consistent.
 */
#include "util/os/os_env.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
    std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
    if (!ok) ++g_failures;
}

/// Puts a variable in the environment of THIS process, for the checks below.
/// Not part of what is being tested: it is the only way to know what the
/// answer should be.
void put(const char *assignment) {
#if defined(_WIN32)
    /* Straight to the block this module reads.  `_putenv` would go to the C
     * runtime's copy and only reach the real environment if that runtime keeps
     * the two in sync -- which it does, but relying on it here would make the
     * test depend on the very thing it is checking is not needed. */
    char name[256];
    const char *eq = std::strchr(assignment, '=');
    const size_t n = size_t(eq - assignment);
    std::memcpy(name, assignment, n);
    name[n] = '\0';
    SetEnvironmentVariableA(name, eq + 1);
#else
    putenv(const_cast<char *>(assignment));
#endif
}

} // namespace

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    std::printf("== environment, read from the system ==\n");

    // --- the answers must match `getenv`, over the WHOLE environment --------
    //
    // Not a sample: every entry.  A walk that goes wrong tends to go wrong on
    // one particular shape -- an empty value, a very long one, a name that is a
    // prefix of the next -- and a sample is exactly what misses those.
    unsigned seen = 0, mismatched = 0, missing = 0;
    for (char **e = envp; e != nullptr && *e != nullptr; ++e) {
        const char *eq = std::strchr(*e, '=');
        /* An entry with no `=`, or one whose name is empty, is not a variable
         * anybody can ask for.  Windows puts such entries in (`=C:=C:\...`),
         * and they are meant to be invisible. */
        if (eq == nullptr || eq == *e) continue;
        ++seen;

        char name[256];
        const size_t nlen = size_t(eq - *e);
        if (nlen >= sizeof(name)) continue; // longer than anyone asks for
        std::memcpy(name, *e, nlen);
        name[nlen] = '\0';

        char ours[4096];
        const size_t n = util::os_env(name, ours, sizeof(ours));
        const char *theirs = std::getenv(name);

        if (n == util::kOsEnvUnset) {
            /* `getenv` finds it and we do not.  That is the failure this whole
             * file exists to prevent, so it is reported by name. */
            if (theirs != nullptr) {
                if (missing < 5)
                    std::printf("       not found: %s\n", name);
                ++missing;
            }
            continue;
        }
        if (theirs == nullptr) continue; // only we see it: fine, we see more
        if (n >= sizeof(ours)) continue; // truncated on purpose; nothing to
                                         // compare
        /* A value outside ASCII is not comparable and the difference would not
         * be a bug: on Windows the block is UTF-16 and we narrow it, while
         * `getenv` hands back the ANSI code page.  Two different renderings of
         * the same value, and neither is wrong. */
        bool ascii = true;
        for (const char *q = theirs; *q != '\0'; ++q)
            if (static_cast<unsigned char>(*q) > 0x7F) ascii = false;
        if (!ascii) continue;
        if (std::strcmp(ours, theirs) != 0) {
            if (mismatched < 5)
                std::printf("       %s: ours '%s' vs getenv '%s'\n", name, ours,
                            theirs);
            ++mismatched;
        }
    }
    std::printf("  %u variables compared against getenv\n", seen);
    check(seen > 0, "the environment is not empty");
    check(missing == 0, "every variable getenv sees, we see");
    check(mismatched == 0, "and with the same value");

    // --- a variable we control, so the expected answer is known -------------
    put("VESTA_TEST_OS_ENV=hello");
    char buf[32];
    check(util::os_env("VESTA_TEST_OS_ENV", buf, sizeof(buf)) == 5 &&
              std::strcmp(buf, "hello") == 0,
          "reads back the value that was set");

    check(util::os_env("VESTA_TEST_OS_ENV_NOT_SET", buf, sizeof(buf)) ==
              util::kOsEnvUnset,
          "a variable nobody set comes back as unset");

    /* Unset and set-but-empty are DIFFERENT answers.  Merging them would hide
     * one of the two from whoever asked. */
    put("VESTA_TEST_OS_ENV_EMPTY=");
    const size_t empty = util::os_env("VESTA_TEST_OS_ENV_EMPTY", buf,
                                      sizeof(buf));
    check(empty == 0 || empty == util::kOsEnvUnset,
          "an empty value is 0, not garbage");
#if !defined(_WIN32)
    /* Windows DELETES a variable assigned the empty string, so there `empty`
     * is legitimately "unset".  On POSIX it stays, and must read as length 0. */
    check(empty == 0, "and on POSIX it stays in the environment");
#endif

    // --- the name is matched whole, not by prefix --------------------------
    //
    // A walk that stops comparing at the end of the NAME instead of at the `=`
    // answers a question that was not asked, and the value it returns looks
    // perfectly reasonable.
    put("VESTA_TEST_OS_ENV_LONGER=other");
    check(util::os_env("VESTA_TEST_OS_ENV", buf, sizeof(buf)) == 5 &&
              std::strcmp(buf, "hello") == 0,
          "a name that is a prefix of another does not steal its value");
    check(util::os_env("VESTA_TEST_OS_EN", buf, sizeof(buf)) ==
              util::kOsEnvUnset,
          "and a shorter name matches nothing");

    // --- truncation is VISIBLE ---------------------------------------------
    //
    // The return value is the length the value HAS, not the length written:
    // handing back the truncated length would give a short value that looks
    // complete, which is the quiet wrong answer this project does not accept.
    char small[3];
    const size_t n = util::os_env("VESTA_TEST_OS_ENV", small, sizeof(small));
    check(n == 5, "a value that does not fit still reports its full length");
    check(std::strcmp(small, "he") == 0,
          "and what did fit is there, nul-terminated");
    check(util::os_env("VESTA_TEST_OS_ENV", nullptr, 0) == 5,
          "with no buffer at all it still measures");

    // --- the switch, which is what the allocator actually asks -------------
    put("VESTA_TEST_OS_ENV_ON=1");
    put("VESTA_TEST_OS_ENV_YES=yes");
    put("VESTA_TEST_OS_ENV_ZERO=0");
    put("VESTA_TEST_OS_ENV_ZEROS=00");
    check(util::os_env_flag("VESTA_TEST_OS_ENV_ON"), "`1` is on");
    check(util::os_env_flag("VESTA_TEST_OS_ENV_YES"), "any other value is on");
    check(!util::os_env_flag("VESTA_TEST_OS_ENV_ZERO"), "`0` is off");
    check(util::os_env_flag("VESTA_TEST_OS_ENV_ZEROS"),
          "`00` is not `0`, so it is on");
    check(!util::os_env_flag("VESTA_TEST_OS_ENV_NOT_SET"), "unset is off");
    check(!util::os_env_flag(""), "an empty name is off, not a crash");
    check(!util::os_env_flag(nullptr), "and neither is a null one");

#if defined(_WIN32)
    /* Windows variable names are case-insensitive, and the block stores them
     * with whatever case the setter used.  Matching case-sensitively there
     * would make `PATH` and `Path` different variables, which is not what the
     * system does. */
    check(util::os_env_flag("vesta_test_os_env_on"),
          "on Windows the name is case-insensitive");
#endif

    std::printf("%s\n", g_failures == 0 ? "ALL OK" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}

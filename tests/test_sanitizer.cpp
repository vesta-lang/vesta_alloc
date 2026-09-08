/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file tests/test_sanitizer.cpp
 * @brief
 * \~english The checking mode, pointed at four mistakes made on purpose.
 * \~spanish El modo comprobacion, apuntado a cuatro fallos hechos a proposito.
 * \~
 *
 * \~english
 * WHAT THIS IS FOR, and it is not "the code runs": a checker is the one kind of
 * program whose failure is SILENCE.  If it stops catching something, everything
 * still builds, every other test still passes, and the report still comes out
 * -- clean.  So the only test worth having is one that commits the mistakes and
 * demands they be found.
 *
 * It runs with `VESTA_ALLOC_SAN_EXITCODE=0` because this program is SUPPOSED to
 * be full of bugs: without that, the checker would do its job and kill the test
 * that was checking it.
 *
 * \~spanish
 * PARA QUE ES ESTO, y no es "que el codigo corra": un comprobador es la unica
 * clase de programa cuyo fallo es el SILENCIO.  Si deja de cazar algo, todo
 * sigue compilando, los demas tests siguen pasando y el informe sigue saliendo
 * -- limpio --.  Asi que el unico test que vale es el que comete los fallos y
 * exige que aparezcan.
 *
 * Corre con `VESTA_ALLOC_SAN_EXITCODE=0` porque este programa DEBE estar lleno
 * de fallos: sin eso, el comprobador haria su trabajo y mataria al test que lo
 * estaba comprobando.
 * \~
 */

#include "util/alloc/host_allocator.h"
#include "util/alloc/sanitizer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

int failures = 0;

void check(bool ok, const char *what) {
    std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
    if (!ok) ++failures;
}

/* Out of line and never inlined, so each mistake has a frame of its own and the
 * report can be read.  A checker whose findings all point at `main` names
 * nothing. */
[[gnu::noinline]] void *allocate_and_forget(size_t n) {
    return util::host_alloc(n);
}
[[gnu::noinline]] void *allocate_to_overflow(size_t n) {
    return util::host_alloc(n);
}

/**
 * @brief Runs this same program again, at the guard level, told to misbehave.
 *
 * IT HAS TO BE A CHILD, and that is the point of the level rather than a
 * nuisance: what it catches, it catches by FAULTING where the mistake is.  A
 * test that made the mistake in its own process would die, and a dead test
 * proves nothing -- so the mistake is made over there and what is checked here
 * is that over there it died.
 *
 * The environment goes on the command line because setting it for a child
 * portably is the one part of this that has no common spelling.
 *
 * @return the child's exit status, or -1 if it could not be run.
 */
int run_child(const char *self, const char *what) {
    char cmd[1024];
#if defined(_WIN32)
    std::snprintf(cmd, sizeof cmd,
                  "set VESTA_ALLOC_SAN=4&& set VESTA_ALLOC_SAN_EXITCODE=0&& "
                  "\"%s\" %s > NUL 2>&1",
                  self, what);
#else
    std::snprintf(cmd, sizeof cmd,
                  "VESTA_ALLOC_SAN=4 VESTA_ALLOC_SAN_EXITCODE=0 "
                  "'%s' %s > /dev/null 2>&1",
                  self, what);
#endif
    return std::system(cmd);
}

} // namespace

int main(int argc, char **argv) {
    /* The child half: make one mistake and let the guard page answer.  If it
     * comes back at all, the level did not do its job, and saying so out loud
     * is what the parent reads. */
    if (argc > 1) {
#if defined(VESTA_ALLOC_SANITIZER) && VESTA_ALLOC_SANITIZER
        if (std::strcmp(argv[1], "guard-overflow") == 0) {
            unsigned char *p =
                static_cast<unsigned char *>(util::host_alloc(64));
            std::memset(p, 0x11, 4096); // far past the end: must fault HERE
            std::printf("the write went through\n");
            return 0; // and a zero here is the failure the parent looks for
        }
        if (std::strcmp(argv[1], "guard-afterfree") == 0) {
            unsigned char *p =
                static_cast<unsigned char *>(util::host_alloc(64));
            util::host_free(p);
            volatile unsigned char v = p[0]; // must fault HERE
            std::printf("the read went through: %u\n", unsigned(v));
            return 0;
        }
        if (std::strcmp(argv[1], "guard-ok") == 0) {
            unsigned char *p =
                static_cast<unsigned char *>(util::host_alloc(64));
            std::memset(p, 0x22, 64); // exactly what was asked for
            unsigned sum = 0;
            for (int i = 0; i < 64; ++i) sum += p[i];
            util::host_free(p);
            return sum == 64u * 0x22 ? 0 : 1;
        }
#endif
        return 0;
    }

    std::printf("== the checking mode ==\n");

#if !defined(VESTA_ALLOC_SANITIZER) || !VESTA_ALLOC_SANITIZER
    std::printf("  the checker is not in this build; nothing to check\n");
    std::printf("TODO OK\n");
    return 0;
#else
    /* The count moves, it is not read as an absolute: the run-time itself
     * allocates, and demanding an exact total would make this test fail the day
     * the C++ library changes its mind about something. */
    const uint64_t before_double = util::san_verdicts();
    void *twice = util::host_alloc(32);
    util::host_free(twice);
    util::host_free(twice); // on purpose
    check(util::san_verdicts() > before_double,
          "a block released twice is caught, in the act");

    const uint64_t before_over = util::san_verdicts();
    unsigned char *over =
        static_cast<unsigned char *>(allocate_to_overflow(24));
    std::memset(over, 0x11, 24 + 2); // two bytes too many, on purpose
    util::host_free(over);
    check(util::san_verdicts() > before_over,
          "a write past the end is caught when the block is released");

    const uint64_t before_uaf = util::san_verdicts();
    unsigned char *after = static_cast<unsigned char *>(util::host_alloc(48));
    util::host_free(after);
    /* NOT byte zero: the first pointer-sized bytes of a released block hold the
     * allocator's own free-list link, so they are not poisoned and cannot be
     * watched.  That is the allocator being right and the checker adapting --
     * and this line is here so the limit is written down where it is felt. */
    after[16] = 0x77;
    void *reused = util::host_alloc(48); // the poison is checked here
    check(util::san_verdicts() > before_uaf,
          "a write after release is caught when the block is handed out again");
    util::host_free(reused);

    /* The leak is LAST because it is the only one reported at exit: what it
     * proves is checked by eye in the report, and here we only make sure there
     * is one to find. */
    void *leaked = allocate_and_forget(40);
    check(leaked != nullptr, "and one block is left behind, for the report");

    /* THE LIFE IS MEASURED, and this is the check that it is measured and not
     * merely printed.  A block kept alive across a known number of allocations
     * has to come out with a life of at least that many.  The first version
     * read a counter that is only filled in when the report is written, so
     * every life was zero and every site in every program was reported
     * "Instant" -- a report that looks exactly like a working one. */
    {
        const uint64_t before = util::san_longest_life();
        void *kept = util::host_alloc(32);
        for (int i = 0; i < 200; ++i) util::host_free(util::host_alloc(16));
        util::host_free(kept);
        const uint64_t now = util::san_longest_life();
        check(now >= 200 && now > before,
              "a block kept alive across 200 allocations measures at least 200");
    }

    /* THE GUARD LEVEL, from a safe distance.  What it catches, it catches by
     * faulting, so the mistakes are made in a child and what is asserted here
     * is that the child did not survive them -- and, just as important, that a
     * child which does nothing wrong comes back fine.  Without that last one,
     * a level that killed everything would look like a level that worked. */
    check(run_child(argv[0], "guard-ok") == 0,
          "at the guard level, ordinary use still works");
    check(run_child(argv[0], "guard-overflow") != 0,
          "a write past the end faults WHERE it happens, not at release");
    check(run_child(argv[0], "guard-afterfree") != 0,
          "and so does a read of a block that was already released");

    std::printf(failures == 0 ? "TODO OK\n" : "HAY FALLOS\n");
    return failures == 0 ? 0 : 1;
#endif
}

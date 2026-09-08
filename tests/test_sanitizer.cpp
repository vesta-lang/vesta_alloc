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

} // namespace

int main() {
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

    std::printf(failures == 0 ? "TODO OK\n" : "HAY FALLOS\n");
    return failures == 0 ? 0 : 1;
#endif
}

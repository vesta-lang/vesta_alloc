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
int skipped = 0;

void check(bool ok, const char *what) {
    std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
    if (!ok) ++failures;
}

#if defined(VESTA_ALLOC_SANITIZER) && VESTA_ALLOC_SANITIZER
/**
 * @brief A row that only means anything from @p needed upwards.
 *
 * THE LEVELS ARE A LADDER AND EACH RUNG BUYS ONE THING.  Poisoning on release
 * is what makes a write after free show up when the block is handed out again;
 * below that rung nothing poisons, so the write leaves no mark and there is
 * nothing to catch.  A row demanding it at the canary level is not finding a
 * hole in the checker, it is asking for a rung that was not asked for.
 *
 * So the row DECLARES the level it needs, and when the level in force is lower
 * it is counted APART.  Never as a pass: a skipped check added to the passes
 * turns "not looked at here" into "fine here", which is how a net stops being
 * one without anybody noticing.
 */
void check_from_level(util::SanLevel needed, bool ok, const char *what) {
    if (util::detail::g_san_level < needed) {
        std::printf("  [SKIP] %s -- needs level %u and level %u is in force\n",
                    what, unsigned(needed),
                    unsigned(util::detail::g_san_level));
        ++skipped;
        return;
    }
    check(ok, what);
}
#endif

/* \~english Out of line and never inlined, so each mistake has a frame of its
 * own and the report can be read.  A checker whose findings all point at `main`
 * names nothing.
 *
 * \~spanish Fuera de linea y nunca inlinadas, para que cada fallo tenga marco
 * propio y el informe se pueda leer.  Un comprobador cuyos hallazgos apuntan
 * todos a `main` no nombra nada.  \~ */
[[gnu::noinline]] void *allocate_and_forget(size_t n) {
    return util::host_alloc(n);
}
[[gnu::noinline]] void *allocate_to_overflow(size_t n) {
    return util::host_alloc(n);
}

/**
 * @brief
 * \~english Runs this same program again, at the guard level, told to
 *           misbehave.
 * \~spanish Vuelve a correr este mismo programa, en el nivel de guarda, con
 *           orden de portarse mal.
 * \~
 *
 * \~english
 * IT HAS TO BE A CHILD, and that is the point of the level rather than a
 * nuisance: what it catches, it catches by FAULTING where the mistake is.  A
 * test that made the mistake in its own process would die, and a dead test
 * proves nothing -- so the mistake is made over there and what is checked here
 * is that over there it died.
 *
 * The environment goes on the command line because setting it for a child
 * portably is the one part of this that has no common spelling.
 *
 * \~spanish
 * TIENE QUE SER UN HIJO, y eso es lo bueno del nivel, no un estorbo: lo que
 * caza, lo caza FALLANDO donde esta el fallo.  Un test que cometiera el fallo
 * en su propio proceso moriria, y un test muerto no prueba nada -- asi que el
 * fallo se comete alli y lo que se comprueba aqui es que alli murio.
 *
 * El entorno va en la linea de ordenes porque ponerselo a un hijo de forma
 * portable es la unica parte de esto que no tiene una forma comun de
 * escribirse.
 * \~
 *
 * @return
 * \~english the child's exit status, or -1 if it could not be run.
 * \~spanish el codigo de salida del hijo, o -1 si no se pudo ejecutar.
 * \~
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
    /* \~english The child half: make one mistake and let the guard page answer.
     * If it comes back at all, the level did not do its job, and saying so out
     * loud is what the parent reads.
     *
     * \~spanish La mitad del hijo: cometer un fallo y dejar que conteste la
     * pagina de guarda.  Si vuelve siquiera, el nivel no hizo su trabajo, y
     * decirlo en voz alta es lo que lee el padre.  \~ */
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
    /* \~english The count moves, it is not read as an absolute: the run-time
     * itself allocates, and demanding an exact total would make this test fail
     * the day the C++ library changes its mind about something.
     *
     * \~spanish La cuenta se MUEVE, no se lee como absoluta: el propio runtime
     * reserva, y exigir un total exacto haria fallar este test el dia que la
     * libreria de C++ cambie de idea sobre algo.  \~ */
    const uint64_t before_double = util::san_verdicts();
    void *twice = util::host_alloc(32);
    util::host_free(twice);
    util::host_free(twice); // on purpose
    check_from_level(util::SanLevel::Track, util::san_verdicts() > before_double,
                     "a block released twice is caught, in the act");

    const uint64_t before_over = util::san_verdicts();
    unsigned char *over =
        static_cast<unsigned char *>(allocate_to_overflow(24));
    std::memset(over, 0x11, 24 + 2); // two bytes too many, on purpose
    util::host_free(over);
    check_from_level(util::SanLevel::Canary, util::san_verdicts() > before_over,
                     "a write past the end is caught when the block is "
                     "released");

    /* \~english AND ONLY BELOW THE GUARD LEVEL, because up there this very
     * mistake is caught AT THE INSTANT: the released block's pages are gone, so
     * the write below would kill THIS process -- which is the level doing its
     * job, not a failure.  What the guard level catches is proved further down,
     * in a child, from a safe distance.  Here the block is still mapped and the
     * poison is what remembers, so the mistake is made now and read when the
     * block is handed out again.
     *
     * \~spanish Y SOLO POR DEBAJO DEL NIVEL DE GUARDA, porque alli arriba este
     * mismo fallo se caza AL INSTANTE: las paginas del bloque soltado ya no
     * estan, asi que la escritura de abajo mataria ESTE proceso -- que es el
     * nivel haciendo su trabajo, no un fallo.  Lo que caza el nivel de guarda
     * se demuestra mas abajo, en un hijo y a distancia.  Aqui el bloque sigue
     * mapeado y quien se acuerda es el veneno, asi que el fallo se comete ahora
     * y se lee al volver a entregar el bloque.  \~ */
    if (util::detail::g_san_level >= util::SanLevel::Guard) {
        std::printf("  [SKIP] a write after release is caught when the block "
                    "is handed out again -- at level %u it is caught at the "
                    "instant instead, which the child half proves\n",
                    unsigned(util::detail::g_san_level));
        ++skipped;
    } else {
    const uint64_t before_uaf = util::san_verdicts();
    unsigned char *after = static_cast<unsigned char *>(util::host_alloc(48));
    util::host_free(after);
    /* \~english NOT byte zero: the first pointer-sized bytes of a released
     * block hold the allocator's own free-list link, so they are not poisoned
     * and cannot be watched.  That is the allocator being right and the checker
     * adapting -- and this line is here so the limit is written down where it
     * is felt.
     *
     * \~spanish NO el byte cero: los primeros bytes de un bloque soltado llevan
     * el enlace de la lista de libres del asignador, asi que no se envenenan y
     * no se pueden vigilar.  Eso es el asignador teniendo razon y el
     * comprobador adaptandose -- y esta linea esta aqui para que el limite
     * quede escrito donde se nota.  \~ */
    after[16] = 0x77;
    void *reused = util::host_alloc(48); // the poison is checked here
    check_from_level(util::SanLevel::Poison, util::san_verdicts() > before_uaf,
                     "a write after release is caught when the block is handed "
                     "out again");
    util::host_free(reused);
    }

    /* \~english The leak is LAST because it is the only one reported at exit:
     * what it proves is checked by eye in the report, and here we only make
     * sure there is one to find.
     *
     * \~spanish La fuga va la ULTIMA porque es la unica que se avisa al salir:
     * lo que prueba se comprueba a ojo en el informe, y aqui solo nos
     * aseguramos de que haya una que encontrar.  \~ */
    void *leaked = allocate_and_forget(40);
    check(leaked != nullptr, "and one block is left behind, for the report");

    /* \~english THE LIFE IS MEASURED, and this is the check that it is measured
     * and not merely printed.  A block kept alive across a known number of
     * allocations has to come out with a life of at least that many.  The first
     * version read a counter that is only filled in when the report is written,
     * so every life was zero and every site in every program was reported
     * "Instant" -- a report that looks exactly like a working one.
     *
     * \~spanish LA VIDA SE MIDE, y esta es la comprobacion de que se mide y no
     * solo se imprime.  Un bloque mantenido vivo a lo largo de un numero
     * conocido de reservas tiene que salir con una vida de al menos esas.  La
     * primera version leia un contador que solo se rellena al escribir el
     * informe, asi que todas las vidas eran cero y todo sitio de todo programa
     * salia como "Instant" -- un informe con el mismo aspecto que uno que
     * funciona.  \~ */
    {
        const uint64_t before = util::san_longest_life();
        void *kept = util::host_alloc(32);
        for (int i = 0; i < 200; ++i) util::host_free(util::host_alloc(16));
        util::host_free(kept);
        const uint64_t now = util::san_longest_life();
        check_from_level(util::SanLevel::Track, now >= 200 && now > before,
                         "a block kept alive across 200 allocations measures "
                         "at least 200");
    }

    /* \~english WHAT A SITE MOVED, which is the figure the leak list cannot
     * give.  Two hundred blocks are taken and RELEASED, so nothing survives:
     * every other number in this checker is blind to them by the time the
     * report runs -- the shadow is indexed by address and forgets a block as
     * soon as its address is used again.  A site that allocated and freed
     * gigabytes looks identical to one that never ran.
     *
     * That is the shape worth catching: a buffer that doubles until it is huge
     * and is then released moves everything and leaks nothing.  So the demand
     * here is that the total SURVIVES the blocks, and it is checked as a
     * difference rather than an absolute -- the run-time allocates too, and an
     * exact figure would fail the day the C++ library changes its mind.
     *
     * \~spanish LO QUE MOVIO UN SITIO, que es la cifra que la lista de fugas no
     * puede dar.  Se cogen doscientos bloques y se SUELTAN, asi que no
     * sobrevive ninguno: para cuando corre el informe, todos los demas numeros
     * de este comprobador son ciegos para ellos -- el sombreado se indexa por
     * direccion y olvida un bloque en cuanto su direccion se reutiliza --.  Un
     * sitio que reservo y libero gigabytes tiene el mismo aspecto que uno que
     * no se ejecuto nunca.
     *
     * Esa es la forma que merece la pena cazar: un buffer que se duplica hasta
     * ser enorme y luego se libera lo mueve todo y no fuga nada.  Asi que aqui
     * se exige que el total SOBREVIVA a los bloques, y se comprueba como
     * diferencia y no como absoluto -- el runtime tambien reserva, y una cifra
     * exacta fallaria el dia que la libreria de C++ cambie de idea.  \~ */
    {
        const uint64_t before = util::san_moved_bytes();
        for (int i = 0; i < 200; ++i) util::host_free(util::host_alloc(512));
        check_from_level(util::SanLevel::Track,
                         util::san_moved_bytes() >= before + 200u * 512u,
                         "lo que un sitio movio se sigue sabiendo despues de "
                         "soltarlo todo, que es lo que la lista de fugas no "
                         "puede decir");
    }

    /* \~english AND A BLOCK PAST THE SMALL-CLASS LIMIT, which used to be
     * dropped one line before its stack was walked.  It has no shadow slot, so
     * it has no life and no leak verdict -- and none of that is needed to say
     * how big it was.  Measured on a real compile, the blocks out here were
     * forty-one operations and the three largest allocations in the program.
     *
     * \~spanish Y UN BLOQUE PASADO EL LIMITE DE CLASE PEQUENA, que antes se
     * tiraba una linea antes de recorrer su pila.  No tiene ranura de sombra,
     * asi que no tiene vida ni veredicto de fuga -- y nada de eso hace falta
     * para decir cuanto media.  Medido en una compilacion de verdad, los
     * bloques de aqui fuera eran cuarenta y una operaciones y las tres mayores
     * reservas del programa.  \~ */
    {
        const uint64_t before = util::san_moved_bytes();
        const size_t big = 64u << 20; // well past any size class
        void *p = util::host_alloc(big);
        check(p != nullptr, "una reserva grande se sirve");
        util::host_free(p);
        check_from_level(util::SanLevel::Track,
                         util::san_moved_bytes() >= before + big,
                         "y un bloque fuera de la region sombreada tambien se "
                         "cuenta, que es donde viven las reservas mayores del "
                         "programa");
    }

    /* \~english THE GUARD LEVEL, from a safe distance.  What it catches, it
     * catches by faulting, so the mistakes are made in a child and what is
     * asserted here is that the child did not survive them -- and, just as
     * important, that a child which does nothing wrong comes back fine.
     * Without that last one, a level that killed everything would look like a
     * level that worked.
     *
     * \~spanish EL NIVEL DE GUARDA, desde una distancia segura.  Lo que caza,
     * lo caza fallando, asi que los fallos se cometen en un hijo y lo que se
     * afirma aqui es que el hijo no sobrevivio a ellos -- y, igual de
     * importante, que un hijo que no hace nada mal vuelve bien.  Sin esto
     * ultimo, un nivel que matara todo pareceria un nivel que funciona.  \~ */
    check(run_child(argv[0], "guard-ok") == 0,
          "at the guard level, ordinary use still works");
    check(run_child(argv[0], "guard-overflow") != 0,
          "a write past the end faults WHERE it happens, not at release");
    check(run_child(argv[0], "guard-afterfree") != 0,
          "and so does a read of a block that was already released");

    /* Apart, never added to the passes: "not looked at here" and "fine here"
     * are different things. */
    if (skipped != 0)
        std::printf("%d SKIPPED because the level in force is lower\n", skipped);
    std::printf(failures == 0 ? "TODO OK\n" : "HAY FALLOS\n");
    return failures == 0 ? 0 : 1;
#endif
}

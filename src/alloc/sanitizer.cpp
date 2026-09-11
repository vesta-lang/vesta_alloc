/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file src/alloc/sanitizer.cpp
 * @brief
 * \~english The checking mode: the shadow, the stack depot and the verdicts.
 * \~spanish El modo comprobacion: el sombreado, el deposito de pilas y los
 *           veredictos.
 * \~
 *
 * \~english
 * WHERE THE MEMORY COMES FROM, and it is the first thing to get right: from
 * `os_alloc`, NEVER from `host_alloc` and never from `malloc`.  Both are the
 * thing being watched, and asking them would call this file again from inside
 * itself.  It is the same rule the call-site table already follows.
 *
 * THE COST PER BLOCK, which is what decides whether this is usable at all:
 * sixteen bytes.  Keeping a whole stack per block would be a hundred and
 * thirty-odd -- eight times a block of the smallest class -- so stacks go into
 * a DEPOT, stored once and looked up by hash, and the shadow keeps a four-byte
 * id.  Real programs have thousands of live blocks and dozens of distinct
 * stacks.
 *
 * ONLY THE SMALL-CLASS REGION IS SHADOWED, and the report SAYS SO.  A checker
 * that quietly does not look somewhere is worse than no checker, because its
 * silence reads as "there is nothing there".  Spans, the big region and the
 * direct blocks are counted apart and named as not covered.
 *
 * \~spanish
 * DE DONDE SALE LA MEMORIA, y es lo primero que hay que acertar: de `os_alloc`,
 * NUNCA de `host_alloc` ni de `malloc`.  Los dos son lo que se esta vigilando,
 * y pedirselo a ellos llamaria a este fichero desde dentro de si mismo.  Es la
 * misma regla que ya sigue la tabla de sitios de llamada.
 *
 * EL COSTE POR BLOQUE, que es lo que decide si esto se puede usar siquiera:
 * dieciseis bytes.  Guardar una pila entera por bloque serian ciento treinta y
 * pico -- ocho veces un bloque de la clase mas pequena --, asi que las pilas van
 * a un DEPOSITO, guardadas una vez y buscadas por hash, y el sombreado se queda
 * con un identificador de cuatro bytes.  Un programa real tiene miles de bloques
 * vivos y decenas de pilas distintas.
 *
 * SOLO SE SOMBREA LA REGION DE CLASES PEQUENAS, y el informe LO DICE.  Un
 * comprobador que calla lo que no mira es peor que no tenerlo, porque su
 * silencio se lee como "ahi no hay nada".  Los tramos, la region grande y los
 * bloques directos se cuentan aparte y se nombran como no cubiertos.
 * \~
 */

#include "util/alloc/sanitizer.h"

#if defined(VESTA_ALLOC_SANITIZER) && VESTA_ALLOC_SANITIZER

#include "util/alloc/host_allocator.h"
#include "util/alloc/host_allocator_layout.h"
#include "util/os/os_env.h"
#include "util/os/os_memory.h"
/* \~english FOR THE NAMES, and only to READ what somebody else installed.  This
 * mode does not own a symbolizer and must not grow one: the allocator's report
 * already takes a resolver and a name formatter from whoever links the library,
 * and using them is what stops the same address coming out as `parse_tokens` in
 * one place and `_ZNSt7__cxx11...` in another.  That exact split is written
 * down in `alloc_csv.h` as a bug that already happened one layer up; this is
 * the same bug one layer down.
 *
 * \~spanish PARA LOS NOMBRES, y solo para LEER lo que instalo otro.  Este modo
 * no tiene simbolizador propio y no debe criar uno: el informe del asignador ya
 * recibe un resolutor y un formateador de nombres de quien enlaza la libreria,
 * y usarlos es lo que evita que la misma direccion salga como `parse_tokens` en
 * un sitio y `_ZNSt7__cxx11...` en otro.  Ese corte exacto esta escrito en
 * `alloc_csv.h` como un fallo que ya paso una capa mas arriba; esto es el mismo
 * fallo una capa mas abajo.  \~ */
#include "util/report/alloc_csv.h"
#include "util/symbols/module_symbols.h"
#include "util/symbols/self_symbols.h"
/* \~english Ours, not the system's: this file already moves memory with the
 * library's own primitives, and a checker calling out to the C library from
 * inside the allocation path is the shape this project avoids everywhere.
 *
 * \~spanish Los nuestros, no los del sistema: este fichero ya mueve memoria con
 * las primitivas de la propia libreria, y un comprobador llamando a la
 * libreria de C desde dentro del camino de reserva es la forma que este
 * proyecto evita en todas partes.  \~ */
#include "util/mem/vesta_memcpy.h"
#include "util/mem/vesta_memset.h"

/* \~english Only for making the directory the tables go in; nothing on the
 * allocation path reaches for these.
 * \~spanish Solo para crear el directorio donde van las tablas; nada del camino
 * de reserva echa mano de esto.  \~ */
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace util {

namespace detail {

SanLevel g_san_level = SanLevel::Off;

} // namespace detail

namespace {

// =========================================================================
//  Knobs, all of them reachable from the environment
// =========================================================================

/// \~english How deep a stack is walked.  Eight is what makes a leak report
///           name the culprit instead of the helper it went through, and it is
///           still one cache line of pointers.
/// \~spanish Cuanto se recorre una pila.  Ocho es lo que hace que un informe de
///           fugas nombre al culpable en vez de al ayudante por el que paso, y
///           sigue siendo una linea de cache de punteros.
/// \~
/// The most frames a stack can ever hold.  It sizes the depot entry, so it is
/// a constant; how many are actually WALKED is @c g_depth.
constexpr unsigned kFrames = 8;

/**
 * @brief
 * \~english How many frames are actually walked.  Two by default.
 * \~spanish Cuantos marcos se recorren de verdad.  Dos por defecto.
 * \~
 *
 * \~english
 * TWO, AND THE NUMBER IS MEASURED.  Depth is not a detail of presentation: the
 * whole stack is the KEY into the depot, so every extra frame multiplies the
 * number of distinct entries -- the same allocation site reached by forty paths
 * becomes forty stacks.  At depth eight a real compile of this project produced
 * **40.203.415 stacks that did not fit** and left **33.158.359 blocks (3,8 GiB)
 * with no site to charge them to**: eighty-four per cent of the program, gone
 * from a report whose whole purpose is attribution.  The failure mode was the
 * right one -- it filled saying so, it did not lie -- but a tree that describes
 * the sixth part that fit is not a tree of the program.
 *
 * WHY TWO IS ENOUGH, and this is the part that decides it rather than taste.
 * The chain that a report cannot see breaks in exactly ONE place: the return
 * address lands inside an out-of-line member of the C++ library --
 * `_M_realloc_insert`, `_M_mutate` -- and the DWARF inline chain ends there
 * because that function was not inlined.  One frame further out is `push_back`,
 * which IS inlined into the caller, so the inline chain already in place names
 * the program from there.  Two frames turn the unattributable half into
 * answers; eight were paying for depth nobody could read anyway.
 *
 * IT IS A KNOB because the right depth depends on the shape of the program, and
 * because a build without `-fno-omit-frame-pointer` gets one frame whatever is
 * asked -- the walk validates every step and stops rather than inventing.
 *
 * \~spanish
 * DOS, Y EL NUMERO ESTA MEDIDO.  La profundidad no es un detalle de
 * presentacion: la pila ENTERA es la CLAVE del deposito, asi que cada marco de
 * mas multiplica el numero de entradas distintas -- el mismo sitio de reserva
 * alcanzado por cuarenta caminos pasa a ser cuarenta pilas.  Con profundidad
 * ocho, una compilacion real de este proyecto produjo **40.203.415 pilas que no
 * cupieron** y dejo **33.158.359 bloques (3,8 GiB) sin sitio al que cargarlos**:
 * el ochenta y cuatro por ciento del programa, fuera de un informe cuyo unico
 * proposito es atribuir.  El modo de fallo era el bueno -- se lleno diciendolo,
 * no mintio -- pero un arbol que describe la sexta parte que cupo no es un
 * arbol del programa.
 *
 * POR QUE DOS BASTA, y esta es la parte que lo decide en vez del gusto.  La
 * cadena que un informe no ve se rompe en UN solo sitio: la direccion de
 * retorno cae dentro de un metodo fuera de linea de la libreria de C++ --
 * `_M_realloc_insert`, `_M_mutate` -- y la cadena de inline del DWARF termina
 * ahi porque esa funcion no se inlino.  Un marco mas afuera esta `push_back`,
 * que SI esta inlinada en quien la llama, asi que la cadena de inline que ya
 * hay nombra al programa desde ese punto.  Dos marcos convierten en respuestas
 * la mitad que no se podia atribuir; ocho pagaban una profundidad que ademas
 * nadie podia leer.
 *
 * ES UN MANDO porque la profundidad buena depende de la forma del programa, y
 * porque una compilacion sin `-fno-omit-frame-pointer` se queda en un marco se
 * pida lo que se pida -- el recorrido valida cada paso y para en vez de
 * inventar.
 * \~
 */
unsigned g_depth = 2;

/// \~english How many distinct stacks fit in the depot.  Overflow is COUNTED,
///           never quietly dropped: a depot that lies because it is full is
///           worse than a small one that says so.
/// \~spanish Cuantas pilas distintas caben en el deposito.  Pasarse se CUENTA,
///           nunca se tira en silencio: un deposito que miente por lleno es
///           peor que uno pequeno que lo dice.
/// \~
/**
 * @brief
 * \~english How many DISTINCT stacks can be remembered.
 * \~spanish Cuantas pilas DISTINTAS se pueden recordar.
 * \~
 *
 * \~english
 * RAISED FROM FOUR THOUSAND, and not on a hunch: a real compile of this project
 * reached 3299 distinct sites -- eighty per cent of the old ceiling -- and the
 * workload it is aimed at is twenty-one modules and several gigabytes.  Past
 * the ceiling `intern_stack` answers zero, and a site that does not fit stops
 * being attributed at all: its blocks fall into the no-site line, which is
 * counted and said, but the one number somebody came looking for is gone.
 *
 * WHAT IT COSTS is the only reason it was not always this: the depot is about
 * eighty bytes an entry and `Life` seventy, so five megabytes each at this
 * size.  That is nothing next to the shadow this mode already keeps over the
 * whole region, and it buys the difference between a list of the top sites and
 * a list of the top sites THAT HAPPENED TO FIT.
 *
 * \~spanish
 * SUBIDO DESDE CUATRO MIL, y no por corazonada: una compilacion de verdad de
 * este proyecto llego a 3299 sitios distintos -- el ochenta por ciento del
 * techo anterior -- y la carga a la que apunta son veintiun modulos y varios
 * gigabytes.  Pasado el techo `intern_stack` contesta cero, y un sitio que no
 * cabe deja de atribuirse del todo: sus bloques caen en la linea de "sin
 * sitio", que se cuenta y se dice, pero el numero que alguien venia a buscar ya
 * no esta.
 *
 * LO QUE CUESTA es la unica razon de que no fuera siempre asi: el deposito son
 * unos ochenta bytes por entrada y `Life` setenta, o sea cinco megabytes cada
 * uno a este tamano.  No es nada al lado del sombreado que este modo ya
 * mantiene sobre la region entera, y compra la diferencia entre una lista de
 * los sitios mayores y una lista de los sitios mayores QUE CUPIERON.
 * \~
 */
constexpr uint32_t kDepotSlots = 1u << 16;

/// \~english The byte a released block is filled with.  A pointer built out of
///           it is wildly unmapped on both systems, and read as a number it is
///           recognisable at a glance in a debugger.
/// \~spanish El byte con el que se llena un bloque soltado.  Un puntero hecho
///           con el cae en memoria sin mapear en los dos sistemas, y leido como
///           numero se reconoce de un vistazo en un depurador.
/// \~
constexpr unsigned char kPoisonByte = 0xDD;

/// \~english The pattern written behind the caller's bytes, and how much of it.
///           Four bytes, so a single off-by-one is caught and no plausible
///           memcpy reproduces it by accident.
/// \~spanish El patron que se escribe detras de los bytes del llamante, y
///           cuanto.  Cuatro bytes, para cazar un desvio de uno y que ningun
///           memcpy plausible lo reproduzca por casualidad.
/// \~
constexpr unsigned char kCanaryByte = 0xAB;
constexpr size_t kCanaryBytes = 4;

/// \~english How much of a released block is poisoned; see @c SanPoison.
/// \~spanish Cuanto de un bloque soltado se envenena; ver @c SanPoison.
/// \~
SanPoison g_poison = SanPoison::Line;

/// \~english Which edge @c SanLevel::Guard puts the block against; see
///           @c SanGuard.
/// \~spanish Contra que borde pone el bloque @c SanLevel::Guard; ver
///           @c SanGuard.
/// \~
SanGuard g_guard_edge = SanGuard::Overflow;

/**
 * @brief
 * \~english What makes the process exit non-zero.  0 = nothing, 1 = proven,
 *           2 = proven and suspected.
 * \~spanish Que hace que el proceso salga distinto de cero.  0 = nada,
 *           1 = demostrado, 2 = demostrado y sospechado.
 * \~
 *
 * \~english
 * ONE IS THE DEFAULT, AND WHICH ONE IT IS MATTERS MORE THAN IT LOOKS.  A block
 * still alive at exit is SUSPECTED, never proven: memory a program keeps on
 * purpose until it dies looks exactly the same.  Failing a build on that was
 * the first thing this mode did, and it took ten of the library's own tests
 * down with it -- every one of them for keeping something alive on purpose.
 *
 * A checker that cries wolf gets switched off and never comes back, so a
 * suspicion is REPORTED and does not gate.  Two is there for a codebase that
 * has decided it wants zero blocks alive at exit and is willing to chase them.
 *
 * \~spanish
 * UNO ES EL DEFECTO, Y CUAL SEA IMPORTA MAS DE LO QUE PARECE.  Un bloque vivo
 * al salir es SOSPECHA, nunca prueba: la memoria que un programa se queda
 * adrede hasta morir tiene ese mismo aspecto.  Hacer fallar un build por eso
 * fue lo primero que hizo este modo, y se llevo por delante diez tests de la
 * propia libreria -- todos por quedarse algo vivo a proposito.
 *
 * Un comprobador que grita en falso se apaga y no vuelve, asi que una sospecha
 * se AVISA y no corta.  El dos esta para quien haya decidido que quiere cero
 * bloques vivos al salir y este dispuesto a perseguirlos.
 * \~
 */
unsigned g_exit_code = 1;

// =========================================================================
//  What the checker found, and what it could not look at
// =========================================================================

std::atomic<uint64_t> g_verdicts{0};  ///< anything worth printing
std::atomic<uint64_t> g_proven{0};    ///< of those, the ones that are FACTS
std::atomic<uint64_t> g_uncovered{0};  ///< blocks outside the shadowed region
std::atomic<uint64_t> g_depot_full{0}; ///< stacks that did not fit
std::atomic<uint64_t> g_no_shadow{0};  ///< rows the system would not give
std::atomic<uint64_t> g_cross_thread{0}; ///< allocated here, released there

/**
 * @brief
 * \~english How sure the checker is, and it travels WITH the verdict.
 * \~spanish Cuanta certeza tiene el comprobador, y viaja CON el veredicto.
 * \~
 *
 * \~english
 * Not decoration.  "Could not prove this is right" is not "proved it is wrong"
 * -- the lesson `may_alias` already taught this codebase the hard way -- and a
 * list that mixes the two gets ignored whole.  A trampled canary is PROVEN; a
 * block still alive at exit may be a leak or may be memory the program keeps on
 * purpose, so it is SUSPECTED.
 *
 * \~spanish
 * No es adorno.  "No pude demostrar que esto este bien" no es "he demostrado
 * que esta mal" -- la leccion que `may_alias` ya le dio a este codigo a base de
 * golpes -- y una lista que mezcla las dos cosas se ignora entera.  Un canario
 * pisado es DEMOSTRADO; un bloque vivo al salir puede ser una fuga o puede ser
 * memoria que el programa se queda adrede, asi que es SOSPECHA.
 * \~
 */
enum class Certainty { Proven, Suspected };

const char *certainty_word(Certainty c) noexcept {
    return c == Certainty::Proven ? "PROVEN" : "SUSPECTED";
}

// =========================================================================
//  The stack depot
// =========================================================================

/// \~english A slot is empty, being filled, or readable.  The middle state
///           exists because a reader must never compare against half-written
///           frames and conclude it has found a different stack.
/// \~spanish Una ranura esta vacia, llenandose, o legible.  El estado de en
///           medio existe porque un lector no puede comparar contra marcos a
///           medio escribir y concluir que ha encontrado otra pila.
/// \~
enum : uint32_t { kSlotEmpty = 0, kSlotClaiming = 1, kSlotReady = 2 };

struct Stack {
    std::atomic<uint32_t> state;
    uint8_t frames; ///< how many entries of `pc` are real
    uint8_t walked; ///< 1 = frame chain followed, 0 = one return address only
    const void *pc[kFrames];
};

Stack *g_depot = nullptr;

/**
 * @brief
 * \~english Whether @p fp can be a frame pointer of the stack we are on.
 * \~spanish Si @p fp puede ser un puntero de marco de la pila en la que
 *           estamos.
 * \~
 *
 * \~english
 * WITHOUT `-fno-omit-frame-pointer` whatever sits in that register is just a
 * number, and following it INVENTS a stack.  An invented stack is worse than
 * one true frame, because it names functions that had nothing to do with it.
 * So every step is validated and the walk stops at the first one that is not:
 * the verdict then says it carries a single frame, which is the truth.
 *
 * Cheap and enough: it has to point up the stack from where we are, be aligned,
 * and not be further away than a stack could plausibly be.
 *
 * \~spanish
 * SIN `-fno-omit-frame-pointer` lo que haya en ese registro es un numero
 * cualquiera, y seguirlo INVENTA una pila.  Una pila inventada es peor que un
 * marco cierto, porque nombra funciones que no tuvieron nada que ver.  Asi que
 * cada paso se valida y el recorrido para en el primero que no: entonces el
 * veredicto dice que trae un solo marco, que es la verdad.
 *
 * Barato y suficiente: tiene que apuntar pila arriba desde donde estamos, estar
 * alineado, y no estar mas lejos de lo que una pila puede estar.
 * \~
 */
[[gnu::always_inline]] inline bool plausible_frame(const void *fp,
                                                   const void *below) noexcept {
    const uintptr_t f = reinterpret_cast<uintptr_t>(fp);
    const uintptr_t b = reinterpret_cast<uintptr_t>(below);
    if ((f & (sizeof(void *) - 1)) != 0) return false; // never misaligned
    if (f <= b) return false;                          // stacks grow downwards
    return (f - b) < (size_t(8) << 20);                // and not by 8 MiB
}

/**
 * @brief
 * \~english Fills @p out with the caller's stack, as deep as it can PROVE.
 * \~spanish Llena @p out con la pila del llamante, tan hondo como pueda
 *           DEMOSTRAR.
 * \~
 *
 * @param out
 * \~english where the frames go; at least @c kFrames of them.
 * \~spanish donde van los marcos; al menos @c kFrames.
 * \~
 * @param first
 * \~english the return address the caller already holds.  It is always frame
 *           zero, and it is the WHOLE answer when there is no frame chain to
 *           follow -- which is the old one-address model, kept as a fallback
 *           and not as a degraded case.
 * \~spanish la direccion de retorno que el llamante ya tiene.  Siempre es el
 *           marco cero, y es la respuesta ENTERA cuando no hay cadena de
 *           marcos que seguir -- que es el modelo viejo de una sola direccion,
 *           conservado como respaldo y no como caso degradado.
 * \~
 * @param walked
 * \~english set to true only if at least one frame came from the chain.
 * \~spanish se pone a true solo si al menos un marco vino de la cadena.
 * \~
 * @return
 * \~english how many frames are real.
 * \~spanish cuantos marcos son de verdad.
 * \~
 */
unsigned walk_stack(const void **out, const void *first, const void *from,
                    bool *walked) noexcept {
    out[0] = first;
    *walked = false;
    unsigned n = 1;

    /* \~english WHERE TO START, and it is given rather than taken, for the same
     * reason the return address is: every frame of the checker that sits
     * between the program and this walk is a frame the walk has to begin ABOVE.
     * Reading `__builtin_frame_address(0)` here worked while the hook was
     * called straight from the allocator; with one door in front of it, the
     * chain began inside the checker and the walk died on the first step -- one
     * frame where there had been four.  From that frame the chain is [saved
     * base pointer][return address], which is what both compilers emit whenever
     * they keep the frame pointer at all.  When they do not, `plausible_frame`
     * throws it out on the first step and the one true address is what is left.
     *
     * \~spanish POR DONDE EMPEZAR, y se le da en vez de tomarlo, por lo mismo
     * que la direccion de retorno: cada marco del comprobador que se interpone
     * entre el programa y este recorrido es un marco POR ENCIMA del cual hay
     * que empezar.  Leer aqui `__builtin_frame_address(0)` valia mientras el
     * gancho se llamaba directo desde el asignador; con una puerta delante, la
     * cadena empezaba dentro del comprobador y el recorrido moria en el primer
     * paso -- un marco donde habia cuatro --.  Desde ese marco la cadena es
     * [base guardada][direccion de retorno], que es lo que emiten los dos
     * compiladores siempre que conserven el puntero de marco.  Cuando no,
     * `plausible_frame` lo tira en el primer paso y queda la unica direccion
     * cierta.  \~ */
    const void *fp = from != nullptr ? from : __builtin_frame_address(0);
    const void *below = fp;
    while (n < g_depth) {
        const void *const *slot = static_cast<const void *const *>(fp);
        const void *const next = slot[0];
        const void *const ret = slot[1];
        if (ret == nullptr || !plausible_frame(next, below)) break;
        /* \~english THE FIRST ONE IS USUALLY THE ONE WE ALREADY HAVE.  Whether
         * the hook ends up as its own frame depends on what the compiler
         * inlined into what, so the chain may start at the very address the
         * caller handed us.  Skipping it by position would be right in one
         * build and wrong in the next; comparing is right in both.
         *
         * \~spanish EL PRIMERO SUELE SER EL QUE YA TENEMOS.  Que el gancho
         * acabe con marco propio depende de que inlinara el compilador dentro
         * de que, asi que la cadena puede empezar justo en la direccion que nos
         * dio el llamante.  Saltarlo por posicion seria correcto en un build y
         * falso en el siguiente; compararlo es correcto en los dos.  \~ */
        if (ret != out[0]) {
            out[n++] = ret;
            *walked = true;
        }
        below = fp;
        fp = next;
    }
    return n;
}

/// \~english FNV-1a over the frames.  The same stack lands on the same slot
///           every run, which is what makes the report reproducible -- and a
///           report that changes between runs cannot gate a build.
/// \~spanish FNV-1a sobre los marcos.  La misma pila cae en la misma ranura en
///           cada corrida, que es lo que hace el informe reproducible -- y un
///           informe que cambia entre corridas no puede cortar un build.
/// \~
uint32_t hash_stack(const void *const *pc, unsigned n) noexcept {
    uint64_t h = 1469598103934665603ull;
    for (unsigned i = 0; i < n; ++i) {
        uint64_t v = reinterpret_cast<uintptr_t>(pc[i]);
        for (unsigned b = 0; b < 8; ++b) {
            h ^= (v & 0xFFu);
            h *= 1099511628211ull;
            v >>= 8;
        }
    }
    return uint32_t(h ^ (h >> 32));
}

bool same_stack(const Stack &s, const void *const *pc, unsigned n) noexcept {
    if (s.frames != n) return false;
    for (unsigned i = 0; i < n; ++i)
        if (s.pc[i] != pc[i]) return false;
    return true;
}

/**
 * @brief
 * \~english Stores a stack and hands back its id, reusing the one already
 *           there.
 * \~spanish Guarda una pila y devuelve su identificador, reusando el que ya
 *           hubiera.
 * \~
 *
 * \~english
 * Open addressing, APPEND ONLY: a full depot never evicts.  Evicting would make
 * two different stacks share an id and the report would name the wrong
 * function, which is the one failure a checker cannot have.  Full is counted
 * (@c g_depot_full) and answered with zero, which the report prints as "stack
 * not available" instead of as somebody's stack.
 *
 * \~spanish
 * Direccionamiento abierto y SOLO ANADIR: un deposito lleno no desaloja jamas.
 * Desalojar haria que dos pilas distintas compartieran identificador y el
 * informe nombraria la funcion equivocada, que es el unico fallo que un
 * comprobador no puede tener.  Lleno se cuenta (@c g_depot_full) y se contesta
 * con cero, que el informe imprime como "pila no disponible" en vez de como la
 * pila de alguien.
 * \~
 *
 * @return
 * \~english the id, or 0 when there is no stack to give.
 * \~spanish el identificador, o 0 cuando no hay pila que dar.
 * \~
 */
uint32_t intern_stack(const void *const *pc, unsigned n, bool walked) noexcept {
    if (g_depot == nullptr) return 0;
    uint32_t i = hash_stack(pc, n) & (kDepotSlots - 1);
    if (i == 0) i = 1; // zero is reserved for "no stack"
    /* \~english BOUNDED, and it was not, which is what made a saturated depot
     * stop the program instead of merely losing detail.  Every allocation
     * interns its stack, so an unbounded scan costs 65.536 probes -- each one
     * comparing up to eight pointers -- from the moment the table fills.
     * Measured by attaching a debugger to a test that had been running for
     * minutes: it was sitting in `san_on_alloc`, here.
     *
     * Thirty-two and not eight: nothing is ever evicted from this table, so the
     * window is what decides how full it gets before it starts refusing, and
     * refusing costs attribution.  At the occupancy this actually sees -- 3.299
     * distinct sites in a real compile, five per cent of the slots -- a lookup
     * takes about one probe, so the bound is a safety valve and not the common
     * path.  Running out is already counted and printed (`g_depot_full`).
     *
     * \~spanish ACOTADO, y no lo estaba, que es lo que hacia que un deposito
     * saturado parase el programa en vez de solo perder detalle.  Toda reserva
     * interna su pila, asi que un barrido sin tope cuesta 65.536 sondeos --
     * cada uno comparando hasta ocho punteros -- desde que la tabla se llena.
     * Medido adjuntando un depurador a un test que llevaba minutos corriendo:
     * estaba parado en `san_on_alloc`, aqui.
     *
     * Treinta y dos y no ocho: de esta tabla no se desaloja nunca nada, asi que
     * la ventana es lo que decide cuanto se llena antes de empezar a rechazar,
     * y rechazar cuesta atribucion.  Con la ocupacion que esto ve de verdad --
     * 3.299 sitios distintos en una compilacion real, el cinco por ciento de
     * las ranuras -- una busqueda son como un sondeo, asi que el tope es una
     * valvula de seguridad y no el camino comun.  Quedarse sin sitio ya se
     * cuenta y se imprime (`g_depot_full`).  \~ */
    constexpr uint32_t kDepotProbe = 32;
    for (uint32_t probe = 0; probe < kDepotProbe; ++probe) {
        Stack &s = g_depot[i];
        uint32_t st = s.state.load(std::memory_order_acquire);
        if (st == kSlotEmpty) {
            uint32_t expected = kSlotEmpty;
            if (s.state.compare_exchange_strong(expected, kSlotClaiming,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
                for (unsigned k = 0; k < n; ++k) s.pc[k] = pc[k];
                s.frames = uint8_t(n);
                s.walked = walked ? 1 : 0;
                s.state.store(kSlotReady, std::memory_order_release);
                return i;
            }
            st = s.state.load(std::memory_order_acquire);
        }
        /* \~english Someone is filling it right now: it cannot be compared yet,
         * and waiting here would be a lock in a path that must not have one.
         * The next slot is as good, at the price of one more entry.
         *
         * \~spanish Alguien la esta llenando ahora mismo: todavia no se puede
         * comparar, y esperar aqui seria un cerrojo en un camino que no puede
         * tenerlo.  La ranura siguiente vale igual, al precio de una entrada
         * mas.  \~ */
        if (st == kSlotReady && same_stack(s, pc, n)) return i;
        i = (i + 1) & (kDepotSlots - 1);
        if (i == 0) i = 1;
    }
    g_depot_full.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

// =========================================================================
//  The shadow
// =========================================================================

/// What is known about one block.  Sixteen bytes; see the file header for why
/// the stacks are ids and not stacks.
struct Slot {
    uint32_t alloc_stack; ///< id in the depot, 0 = none
    uint32_t free_stack;  ///< id in the depot, 0 = none
    uint32_t req;         ///< what the caller asked for, before the canary
    uint32_t meta;        ///< state and the two thread ids; see below
    /**
     * @brief
     * \~english When it was born, counted in ALLOCATIONS of its own thread.
     * \~spanish Cuando nacio, contado en RESERVAS de su propio hilo.
     * \~
     *
     * \~english
     * NOT A CLOCK, and that is what makes it worth having: two runs of the same
     * program give the same number, so a life can be compared between them and
     * the report can gate a build.  Wall time would answer differently every
     * run and could gate nothing.
     *
     * The counter is one the allocator ALREADY keeps, so measuring lives adds
     * no state anywhere.  That is the condition this whole mode was built
     * under: what the checker needs, the checker pays for, and only in its own
     * build.
     *
     * \~spanish
     * NO ES UN RELOJ, y eso es lo que lo hace valer: dos corridas del mismo
     * programa dan el mismo numero, asi que una vida se puede comparar entre
     * ellas y el informe puede cortar un build.  El reloj de pared contestaria
     * distinto cada vez y no podria cortar nada.
     *
     * El contador es uno que el asignador YA lleva, asi que medir vidas no
     * anade estado en ningun sitio.  Es la condicion bajo la que se construyo
     * todo este modo: lo que el comprobador necesita lo paga el comprobador, y
     * solo en su propio build.
     * \~
     */
    uint32_t seq;
};
static_assert(sizeof(Slot) == 20, "the shadow costs 20 bytes per block");

enum : uint32_t { kStNever = 0, kStAlive = 1, kStFreed = 2 };

[[gnu::always_inline]] inline uint32_t meta_of(uint32_t state, uint32_t at,
                                               uint32_t ft) noexcept {
    return (state & 3u) | ((at & 0x7FFFu) << 2) | ((ft & 0x7FFFu) << 17);
}
[[gnu::always_inline]] inline uint32_t meta_state(uint32_t m) noexcept {
    return m & 3u;
}
[[gnu::always_inline]] inline uint32_t meta_alloc_thread(uint32_t m) noexcept {
    return (m >> 2) & 0x7FFFu;
}
[[gnu::always_inline]] inline uint32_t meta_free_thread(uint32_t m) noexcept {
    return (m >> 17) & 0x7FFFu;
}

/* \~english One row per chunk, indexed by the offset inside the chunk in units
 * of `kAlign`.  Indexing by ALIGNMENT and not by size class costs slots -- a
 * 48-byte block uses one of every three -- and buys not having to track which
 * class a chunk currently serves, which changes when a chunk is recycled.  A
 * checker that gets confused by recycling is a checker that accuses the wrong
 * line.
 *
 * \~spanish Una fila por trozo, indexada por el desplazamiento dentro del trozo
 * en unidades de `kAlign`.  Indexar por ALINEACION y no por clase de tamano
 * gasta ranuras -- un bloque de 48 usa una de cada tres -- y compra no tener
 * que seguir que clase sirve un trozo ahora mismo, que cambia cuando el trozo
 * se recicla.  Un comprobador al que el reciclado confunde es un comprobador
 * que acusa a la linea equivocada.  \~ */
constexpr uint32_t kSlotsPerChunk = kChunkBytes / kAlign;

std::atomic<Slot *> *g_rows = nullptr; ///< one entry per chunk of the region
uint32_t g_row_count = 0;

/**
 * @brief
 * \~english The slot of @p p, creating its row the first time.
 * \~spanish La ranura de @p p, creando su fila la primera vez.
 * \~
 *
 * @return
 * \~english nullptr when @p p is not a small-class block of the region -- a
 *           span, the big region, a direct block, or not ours at all.  That
 *           answer is COUNTED, because it is exactly what the report has to
 *           admit it did not look at.
 * \~spanish nulo cuando @p p no es un bloque de clase pequena de la region --
 *           un tramo, la region grande, un bloque directo, o no nuestro.  Esa
 *           respuesta se CUENTA, porque es justo lo que el informe tiene que
 *           reconocer que no miro.
 * \~
 */
Slot *slot_of(const void *p, size_t *block_bytes) noexcept {
    if (!in_region(p)) return nullptr;
    ChunkHeader *h = chunk_of(const_cast<void *>(p));
    if (h->magic != kChunkMagic) return nullptr; // a span, not class blocks
    /* \~english The size of the BLOCK, which is not what the caller asked for.
     * Poisoning and the canary both need it: writing `req + something` bytes
     * would run into the next block, and a checker that corrupts memory is
     * worse than no checker at all.
     *
     * \~spanish El tamano del BLOQUE, que no es lo que pidio quien llama.  Lo
     * necesitan el veneno y el canario: escribir `req + algo` bytes se meteria
     * en el bloque siguiente, y un comprobador que corrompe memoria es peor que
     * no tener ninguno.  \~ */
    if (block_bytes != nullptr) *block_bytes = kSizes[h->cls];

    const uintptr_t base = detail::g_region_base.load(std::memory_order_relaxed);
    const uintptr_t chunk = reinterpret_cast<uintptr_t>(h);
    const uint32_t row = uint32_t((chunk - base) / kChunkBytes);
    if (row >= g_row_count) return nullptr;

    Slot *slots = g_rows[row].load(std::memory_order_acquire);
    if (slots == nullptr) {
        /* From the SYSTEM, never from `host_alloc`: that is what is being
         * watched, and asking it here would re-enter this function. */
        void *mem = os_alloc(size_t(kSlotsPerChunk) * sizeof(Slot),
                             kOsReadWrite);
        if (mem == nullptr) {
            g_no_shadow.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        std::memset(mem, 0, size_t(kSlotsPerChunk) * sizeof(Slot));
        Slot *expected = nullptr;
        if (!g_rows[row].compare_exchange_strong(expected,
                                                 static_cast<Slot *>(mem),
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
            os_free(mem, size_t(kSlotsPerChunk) * sizeof(Slot));
            slots = expected; // another thread got there first
        } else {
            slots = static_cast<Slot *>(mem);
        }
    }
    const uintptr_t off = reinterpret_cast<uintptr_t>(p) - chunk;
    const uint32_t idx = uint32_t(off / kAlign);
    if (idx >= kSlotsPerChunk) return nullptr;
    return &slots[idx];
}

// =========================================================================
//  What each site turns out to BE, measured instead of declared
// =========================================================================

/**
 * @brief
 * \~english What the blocks of one site did, added up.
 * \~spanish Lo que hicieron los bloques de un sitio, sumado.
 * \~
 *
 * \~english
 * THIS IS THE POINT OF THE WHOLE SHADOW, and it is worth saying plainly: the
 * two axes the allocator lets a caller DECLARE -- how long a block lives and
 * whether it grows -- come out of here MEASURED.  A declaration then stops
 * being the source of truth and becomes a claim that can be checked against the
 * data, which is what `@complexity` is to cost.
 *
 * And it matters more than it looks, because almost nothing declares anything:
 * the whole VM reports `unknown`.  Measuring does not need the hundred sites to
 * be visited one by one.
 *
 * \~spanish
 * ESTO ES PARA LO QUE ESTA EL SOMBREADO ENTERO, y conviene decirlo claro: los
 * dos ejes que el asignador deja DECLARAR a quien llama -- cuanto vive un
 * bloque y si crece -- salen de aqui MEDIDOS.  Una declaracion deja entonces de
 * ser la fuente de verdad y pasa a ser una afirmacion contrastable con los
 * datos, que es lo que `@complexity` es para el coste.
 *
 * Y pesa mas de lo que parece, porque casi nadie declara nada: la VM entera
 * sale como `unknown`.  Medir no exige visitar los cien sitios uno a uno.
 * \~
 */
struct Life {
    uint64_t births;   ///< blocks of this site that were ever handed out
    uint64_t bytes;    ///< what they asked for, added up over the WHOLE run
    /**
     * @brief
     * \~english The part of the above the shadow could not look at.
     * \~spanish La parte de lo de arriba que el sombreado no pudo mirar.
     * \~
     *
     * \~english
     * A BOOLEAN WOULD BE WRONG, and the case that proves it is the one worth
     * finding: a vector that doubles starts inside the small-class region and
     * ends outside it, so the SAME site is on both sides of the boundary --
     * and the doublings that matter are the late, huge ones.  A per-site flag
     * would have to pick a side and would pick the wrong one.
     *
     * What this buys is a column that says how much of a site's volume was
     * merely COUNTED against how much was inspected.  Blocks out there have no
     * shadow slot, so they have no life, no shape and no leak verdict -- and a
     * report that showed their bytes next to everyone else's without saying so
     * would be claiming a coverage it does not have.
     *
     * \~spanish
     * UN BOOLEANO ESTARIA MAL, y el caso que lo demuestra es justo el que
     * merece la pena encontrar: un vector que se duplica empieza dentro de la
     * region de clases pequenas y acaba fuera, asi que el MISMO sitio esta a
     * los dos lados de la frontera -- y las duplicaciones que importan son las
     * ultimas, las enormes.  Una marca por sitio tendria que elegir un lado, y
     * elegiria el equivocado.
     *
     * Lo que esto compra es una columna que dice cuanto del volumen de un sitio
     * solo se CONTO frente a cuanto se inspecciono.  Los bloques de ahi fuera
     * no tienen ranura de sombra, asi que no tienen vida, ni forma, ni
     * veredicto de fuga -- y un informe que ensenara sus bytes al lado de los
     * demas sin decirlo estaria afirmando una cobertura que no tiene.
     * \~
     */
    uint64_t outside_births;
    uint64_t outside_bytes;
    uint64_t deaths;   ///< blocks of this site that were released
    uint64_t life_sum; ///< total life, in allocations of the owning thread
    uint64_t unknown;  ///< released on another thread: lives not comparable
    uint32_t life_max;
    uint32_t size_min;
    uint32_t size_max;
};

Life *g_life = nullptr;

/**
 * @brief
 * \~english One PAIR: the site that handed a block out and the site that gave
 *           it back.
 * \~spanish Un PAR: el sitio que entrego un bloque y el que lo devolvio.
 * \~
 *
 * \~english
 * WHAT A PAIR ANSWERS THAT A SITE ON ITS OWN CANNOT: whether a site has ONE
 * owner.  Per-site figures say how much a place allocates and how long its
 * blocks live; neither says who ends up responsible for them.  A site whose
 * blocks always come back through the same place has an owner and can be sent
 * to an arena of its own; a site whose blocks come back through fifteen is
 * shared, and belongs on the common path.
 *
 * That distinction is the one thing routing needs and the only one nothing here
 * measured -- and it is worth saying plainly that the answer this produces is
 * "yes, one owner" or "no, several", never "probably": NOT being able to show a
 * site has a single owner is not showing that it has several, and a site that
 * is not known stays where it is.
 *
 * The other half comes free with it: where ownership CROSSES a boundary --
 * allocated by the parser, released by the emitter -- which is a thing nobody
 * can see today at all.
 *
 * \~spanish
 * LO QUE CONTESTA UN PAR Y NO PUEDE CONTESTAR UN SITIO SOLO: si un sitio tiene
 * UN dueño.  Las cifras por sitio dicen cuanto reserva un sitio y cuanto viven
 * sus bloques; ninguna dice quien acaba respondiendo por ellos.  Un sitio cuyos
 * bloques vuelven siempre por el mismo sitio tiene dueño y se puede mandar a
 * una arena propia; uno cuyos bloques vuelven por quince es compartido, y su
 * lugar es el camino comun.
 *
 * Esa distincion es lo unico que el encaminado necesita y lo unico que aqui no
 * se medía -- y conviene decir claro que lo que esto produce es "si, un dueño"
 * o "no, varios", nunca "seguramente": NO poder demostrar que un sitio tiene un
 * solo dueño no es demostrar que tiene varios, y un sitio que no se sabe se
 * queda donde esta.
 *
 * La otra mitad viene de regalo: donde la propiedad CRUZA una frontera --
 * reservado por el analizador, soltado por el emisor --, que es algo que hoy no
 * se ve de ninguna manera.  \~
 */
struct Pair {
    /// \~english `(alloc << 32) | free`; 0 = free slot.  \~spanish `(reserva <<
    /// 32) | liberacion`; 0 = ranura libre.  \~
    std::atomic<uint64_t> key;
    uint64_t blocks; ///< \~english how many went this way.  \~spanish cuantos
                     ///< fueron por aqui.  \~
    uint64_t bytes;  ///< \~english and how much they were.  \~spanish y cuanto
                     ///< median.  \~
};

/**
 * @brief
 * \~english Slots for the pairs.  Same size and same discipline as the depot.
 * \~spanish Ranuras de los pares.  Mismo tamano y misma disciplina que el
 *           deposito.
 * \~
 *
 * \~english
 * SIZED FROM A MEASUREMENT, not from a guess.  A real compile through the whole
 * pipeline interned 3.575 distinct stacks, 5,5 % of the depot, without
 * overflowing -- so a site can be one of at most that many on either side.  And
 * there is a hard ceiling above that: a pair only exists when a block is
 * RELEASED, so distinct pairs can never outnumber releases.
 *
 * What that does not settle is a big compile, where releases run into the
 * millions.  Distinct pairs saturate long before that -- nearly everything
 * comes back through a handful of generic places -- but "nearly" is not a
 * measurement, which is why @c g_pair_full exists and is printed.  A table that
 * quietly stops recording turns "these are the pairs" into "these are the pairs
 * that fit", and the two read exactly the same.
 *
 * \~spanish
 * DIMENSIONADO CON UNA MEDIDA, no con una suposicion.  Una compilacion real con
 * la tuberia entera interno 3.575 pilas distintas, el 5,5 % del deposito, sin
 * desbordarlo -- asi que un sitio solo puede ser uno de esos, a cada lado.  Y
 * por encima hay un techo duro: un par solo existe cuando un bloque se SUELTA,
 * asi que los pares distintos no pueden ser mas que las liberaciones.
 *
 * Lo que eso no zanja es una compilacion grande, donde las liberaciones son
 * millones.  Los pares distintos se saturan mucho antes -- casi todo vuelve por
 * un punado de sitios genericos -- pero "casi" no es una medida, y por eso
 * existe @c g_pair_full y por eso se imprime.  Una tabla que deja de apuntar en
 * silencio convierte "estos son los pares" en "estos son los pares que
 * cupieron", y las dos cosas se leen igual.  \~
 */
/* \~english THE TWO GO TOGETHER, and that is why the count is written as a
 * shift.  The index comes out of the TOP bits of the product, so how many to
 * take depends on how big the table is: taking sixteen of them into a table of
 * a million means only the first sixteenth of it is ever reachable, the load
 * factor is sixteen times what it looks like, and growing the table changes
 * nothing at all.  Which is exactly what happened here -- a table of 1.048.576
 * refused 8.970 pairs while holding 38.207, an impossible 3,6 % until you see
 * that the real figure was 58 %.
 *
 * \~spanish LOS DOS VAN JUNTOS, y por eso la cuenta se escribe como un
 * desplazamiento.  El indice sale de los bits ALTOS del producto, asi que
 * cuantos coger depende de lo grande que sea la tabla: coger dieciseis para una
 * tabla de un millon deja alcanzable solo su primera dieciseisava parte, el
 * factor de carga es dieciseis veces el que aparenta, y agrandarla no cambia
 * nada.  Que es justo lo que paso aqui -- una tabla de 1.048.576 nego 8.970
 * pares teniendo 38.207, un 3,6 % imposible hasta que se ve que la cifra real
 * era el 58 %.  \~ */
constexpr uint32_t kPairBits = 20;
constexpr uint32_t kPairSlots = 1u << kPairBits;

/// \~english One allocating site, once the pairs have been grouped: how many
///           different places gave its blocks back, and how much went that way.
///           Built by the report in a single pass, never kept.
/// \~spanish Un sitio de reserva, ya agrupados los pares: cuantos sitios
///           distintos devolvieron sus bloques y cuanto fue por ahi.  Lo
///           construye el informe en una sola pasada y no se guarda.  \~
struct Owned {
    uint32_t owners;    ///< distinct sites that released its blocks
    uint32_t one_owner; ///< the only one, when `owners == 1`
    uint64_t blocks;
    uint64_t bytes;
};

/// \~english The window, like everywhere else here.  \~spanish La ventana, como
/// en todo lo demas de aqui.  \~
constexpr uint32_t kPairProbe = 8;

Pair *g_pairs = nullptr;

/// \~english Releases whose pair found no room: counted, never dropped quietly.
/// \~spanish Liberaciones cuyo par no encontro sitio: contadas, nunca tiradas
/// en silencio.  \~
std::atomic<uint64_t> g_pair_full{0};

/// The longest life anybody has recorded.  Read by @c san_longest_life, which
/// exists so a test can demand that the clock is running at all.
std::atomic<uint64_t> g_longest_life{0};

/**
 * @brief
 * \~english How many blocks THIS thread has been handed, ever.
 * \~spanish Cuantos bloques se le han entregado a ESTE hilo, en total.
 * \~
 *
 * \~english
 * The clock a life is measured against, and it is one the allocator already
 * keeps -- so measuring lives adds no state to anything, which was the
 * condition.  It is the SUM of the per-purpose counters and not a total of its
 * own: the total was replaced by that table precisely so counting by purpose
 * would cost nothing extra, and there is no separate running total left to
 * read.  Sixteen adds, in a mode that is not racing anybody.
 *
 * Looking for a field called `small_allocs` and using it is what the first
 * version did.  That one is filled in at REPORT time by adding this same table
 * up, so during the run it is zero -- and every life came out zero, with the
 * report calmly declaring every site in the program "Instant".
 *
 * \~spanish
 * El reloj contra el que se mide una vida, y es uno que el asignador YA lleva
 * -- asi que medir vidas no anade estado a nada, que era la condicion --.  Es
 * la SUMA de los contadores por proposito y no un total propio: el total se
 * sustituyo por esa tabla justamente para que contar por proposito no costara
 * nada extra, y no queda ningun total corriente aparte que leer.  Dieciseis
 * sumas, en un modo que no compite con nadie.
 *
 * Buscar un campo llamado `small_allocs` y usarlo es lo que hizo la primera
 * version.  Ese se rellena al ESCRIBIR el informe, sumando esta misma tabla,
 * asi que durante la corrida vale cero -- y todas las vidas salian cero, con el
 * informe declarando tan tranquilo que todo sitio del programa era "Instant".
 * \~
 */
/**
 * @brief
 * \~english Blocks THIS mode served, per thread, so a life can still be
 *           measured where the allocator did not carve anything.
 * \~spanish Bloques que sirvio ESTE modo, por hilo, para que una vida se pueda
 *           medir alli donde el asignador no recorto nada.
 * \~
 *
 * \~english
 * ITS OWN, AND KEYED BY THE ALLOCATOR'S THREAD ID.  The allocator counts what
 * it carved; at the guard level it carves nothing, so the count a life is
 * measured against never moved and every block came out as having lived zero
 * allocations.  Adding a field to the allocator's cache to fix it would be the
 * checker charging the allocator for its own telemetry, which is the wrong way
 * round -- so the count lives here and the id is the only thing borrowed.
 *
 * A plain array and not a thread variable, because on Windows a thread variable
 * inside a `malloc` is a recursion waiting to happen -- the runtime's own
 * emulation allocates the first time a thread touches one.  Sized by the same
 * cap the allocator uses for its caches, so an id always fits.
 *
 * \~spanish
 * PROPIA, E INDEXADA POR EL ID DE HILO DEL ASIGNADOR.  El asignador cuenta lo
 * que recorto el; en el nivel de guarda no recorta nada, asi que la cuenta
 * contra la que se mide una vida no se movia nunca y todos los bloques salian
 * habiendo vivido cero reservas.  Anadir un campo a la cache del asignador para
 * arreglarlo seria el comprobador cobrandole al asignador su propia telemetria,
 * que es al reves de como tiene que ser -- asi que la cuenta vive aqui y lo
 * unico prestado es el id.
 *
 * Un array y no una variable de hilo, porque en Windows una variable de hilo
 * dentro de un `malloc` es una recursion esperando a pasar -- la emulacion del
 * runtime reserva la primera vez que un hilo toca una.  Dimensionada con el
 * mismo tope que usa el asignador para sus caches, asi que un id siempre cabe.
 * \~
 */
std::atomic<uint32_t> g_guard_thread_allocs[util::kMaxThreads];

[[gnu::always_inline]] inline uint32_t thread_allocs(
    const detail::ThreadCache *c) noexcept {
    if (!detail::have_cache(c)) return 0;
    uint64_t n = 0;
    for (unsigned i = 0; i < VESTA_ALLOC_TAG_SLOTS; ++i) n += c->stats.by_tag[i];
    /* AND WHAT THIS MODE SERVED ITSELF.  Below the guard level the term is zero
     * and this is the allocator's own figure, unchanged. */
    if (c->id < util::kMaxThreads)
        n += g_guard_thread_allocs[c->id].load(std::memory_order_relaxed);
    return uint32_t(n);
}

/// Below this many allocations of its own thread, a block counts as instant.
uint32_t g_life_instant = 100;
/// And below this one, as medium.  Above, long.
uint32_t g_life_medium = 100000;

/// What that site IS, said in the same words a caller would have used to
/// declare it -- so the two can be put side by side.
const char *use_word(const Life &l) noexcept {
    if (l.deaths == 0) return "Long";
    const uint64_t avg = l.life_sum / l.deaths;
    if (avg < g_life_instant) return "Instant";
    if (avg < g_life_medium) return "Medium";
    return "Long";
}

/// One size for every block is a buffer that is what it is; several is one that
/// grew.  The allocator's own axis, read off the data instead of asked for.
const char *shape_word(const Life &l) noexcept {
    return l.size_min == l.size_max ? "Fixed" : "Growing";
}

/* \~english BLOCKS WHOSE STACK DID NOT FIT, counted apart so the total can be
 * honest.  The depot holds `kDepotSlots` distinct stacks; past that
 * `intern_stack` answers zero, and a volume attributed to stack zero would be a
 * pile of unrelated sites wearing one name.  So they are left out of the
 * per-site figures and said as their own line -- the difference between a total
 * that is short and a total that is short WITHOUT SAYING SO.
 *
 * \~spanish BLOQUES CUYA PILA NO CUPO, contados aparte para que el total pueda
 * ser honesto.  El deposito guarda `kDepotSlots` pilas distintas; pasado eso
 * `intern_stack` contesta cero, y un volumen atribuido a la pila cero seria un
 * monton de sitios sin relacion bajo un solo nombre.  Asi que se quedan fuera
 * de las cifras por sitio y se dicen en su propia linea -- la diferencia entre
 * un total que se queda corto y uno que se queda corto SIN DECIRLO.  \~ */
uint64_t g_birth_lost = 0;
uint64_t g_birth_lost_bytes = 0;

/* \~english THE CHECKER MEASURING ITSELF, which it was doing.  Writing the
 * report resolves every address to a name, and resolving reads DWARF, and
 * reading DWARF allocates -- through `operator new`, in blocks big enough to
 * fall outside the shadow.  Those landed in the very figures being printed, so
 * the totals came out different depending on whether tables had been asked for:
 * measured here at 42 MB in 27 blocks appearing between the text report and the
 * CSV of the SAME run.
 *
 * A measurement that changes because you looked at it is not a measurement.  So
 * from the moment the report starts, what this mode allocates is counted APART
 * and said, rather than mixed into the program's traffic or quietly dropped.
 *
 * \~spanish EL COMPROBADOR MIDIENDOSE A SI MISMO, que es lo que hacia.
 * Escribir el informe resuelve cada direccion a un nombre, y resolver lee
 * DWARF, y leer DWARF reserva -- por `operator new`, en bloques lo bastante
 * grandes como para caer fuera del sombreado.  Eso aterrizaba en las mismas
 * cifras que se estaban imprimiendo, asi que los totales salian distintos segun
 * se hubieran pedido tablas o no: medido aqui en 42 MB en 27 bloques que
 * aparecian entre el informe de texto y el CSV de la MISMA corrida.
 *
 * Una medida que cambia porque la miras no es una medida.  Asi que desde que
 * empieza el informe, lo que este modo reserva se cuenta APARTE y se dice, en
 * vez de mezclarse con el trafico del programa o perderse en silencio.  \~ */
bool g_in_report = false;
uint64_t g_report_births = 0;
uint64_t g_report_bytes = 0;

/**
 * @brief
 * \~english Records that one block was handed out.  Called where it is born.
 * \~spanish Apunta que se entrego un bloque.  Se llama donde nace.
 * \~
 *
 * \~english
 * THE TWIN OF @c note_death, and it exists because the two answer different
 * questions.  The shadow is indexed by ADDRESS, so a slot is reused the moment
 * its address is, and by the end of the run it only remembers what is still
 * alive: it can say what LEAKED and it cannot say what a site ALLOCATED.  Those
 * come apart hard -- a buffer that doubles twenty times and is released moves
 * gigabytes and leaves nothing behind, so it is invisible to every figure this
 * checker had until now, and it is exactly the shape worth finding.
 *
 * IT COSTS TWO ADDITIONS ON AN ID THAT WAS ALREADY COMPUTED.  The site is
 * interned one line earlier because the shadow needs it anyway, and the walk
 * that produced it was already paid.  Nothing new is stored per block, nothing
 * new is allocated, and the allocator is not touched: this whole thing lives
 * inside the checker's own build, which is the condition it was built under.
 *
 * \~spanish
 * EL GEMELO DE @c note_death, y existe porque las dos contestan preguntas
 * distintas.  El sombreado se indexa por DIRECCION, asi que una ranura se
 * reutiliza en cuanto se reutiliza su direccion, y al acabar la corrida solo
 * recuerda lo que sigue vivo: puede decir que se FUGO y no puede decir cuanto
 * RESERVO un sitio.  Y se separan mucho -- un buffer que se duplica veinte
 * veces y se libera mueve gigabytes y no deja nada detras, asi que es invisible
 * para todas las cifras que este comprobador tenia hasta ahora, y es justo la
 * forma que merece la pena encontrar.
 *
 * CUESTA DOS SUMAS SOBRE UN IDENTIFICADOR YA CALCULADO.  El sitio se interna
 * una linea antes porque el sombreado lo necesita igual, y el recorrido que lo
 * produjo ya estaba pagado.  No se guarda nada nuevo por bloque, no se reserva
 * nada nuevo, y el asignador no se toca: todo esto vive dentro del build propio
 * del comprobador, que es la condicion bajo la que se hizo.
 * \~
 */
void note_birth(uint32_t stack, size_t req, bool inspected) noexcept {
    if (g_life == nullptr) return;
    /* Ours, from the report itself.  Kept out of the program's figures and
     * said in the summary; see `g_in_report`. */
    if (g_in_report) {
        g_report_births += 1;
        g_report_bytes += req;
        return;
    }
    if (stack == 0 || stack >= kDepotSlots) {
        g_birth_lost += 1;
        g_birth_lost_bytes += req;
        return;
    }
    Life &l = g_life[stack];
    l.births += 1;
    l.bytes += req;
    /* \~english INSPECTED, not "in the shadow": a guarded block lives on its own
     * pages and still has a table watching it, so it counts as looked at.  What
     * this column is about is coverage, not which structure holds the entry.
     *
     * \~spanish INSPECCIONADO, no "en el sombreado": un bloque con guarda vive
     * en paginas propias y aun asi tiene una tabla vigilandolo, asi que cuenta
     * como mirado.  Esta columna va de cobertura, no de que estructura guarda
     * la entrada.  \~ */
    if (!inspected) {
        l.outside_births += 1;
        l.outside_bytes += req;
    }
}

/// Records what one block did with its life.  Called where the block dies,
/// which is the only place that knows.
void note_death(uint32_t stack, uint32_t req, bool same_thread,
                uint32_t born, uint32_t now) noexcept {
    if (g_life == nullptr || stack == 0 || stack >= kDepotSlots) return;
    Life &l = g_life[stack];
    if (l.deaths == 0 && l.unknown == 0) {
        l.size_min = req;
        l.size_max = req;
    } else {
        if (req < l.size_min) l.size_min = req;
        if (req > l.size_max) l.size_max = req;
    }
    /* \~english A LIFE ONLY MEANS SOMETHING WITHIN ONE THREAD: the counter is
     * that thread's, so subtracting one thread's from another's would produce a
     * number that looks like a life and is not one.  Those are counted apart
     * rather than folded in, which is the difference between "we do not know"
     * and "we know something wrong".
     *
     * \~spanish UNA VIDA SOLO SIGNIFICA ALGO DENTRO DE UN HILO: el contador es
     * de ese hilo, asi que restar el de uno al de otro daria un numero con
     * pinta de vida que no lo es.  Esas se cuentan aparte en vez de mezclarlas,
     * que es la diferencia entre "no lo se" y "se algo equivocado".  \~ */
    if (!same_thread || now < born) {
        ++l.unknown;
        return;
    }
    const uint32_t life = now - born;
    ++l.deaths;
    l.life_sum += life;
    if (life > l.life_max) l.life_max = life;
    uint64_t top = g_longest_life.load(std::memory_order_relaxed);
    while (life > top && !g_longest_life.compare_exchange_weak(
                             top, life, std::memory_order_relaxed))
        ;
}

/**
 * @brief
 * \~english Notes that a block from @p alloc_stack came back through @p
 *           free_stack.
 * \~spanish Apunta que un bloque de @p alloc_stack volvio por @p free_stack.
 * \~
 *
 * \~english
 * CALLED WHERE THE BLOCK DIES, from BOTH doors -- the shadow's and the guarded
 * one.  Only one of the two would be the same mistake this library has now made
 * four times: the strictest level serves blocks the shadow never sees, so a
 * tally wired to the shadow alone goes quiet exactly where the checking is
 * strictest, and goes quiet WITHOUT saying so.
 *
 * Neither id can be zero for the pair to mean anything: zero is what the depot
 * answers when a stack did not fit, and pairing a real site with "no idea"
 * would put a row in the table that looks like knowledge.  Those are left out
 * and the depot already counts its own overflow.
 *
 * \~spanish
 * LLAMADA DONDE MUERE EL BLOQUE, desde las DOS puertas -- la del sombreado y la
 * del bloque con guarda.  Solo una de las dos seria el mismo fallo que esta
 * libreria lleva cometido cuatro veces: el nivel mas estricto sirve bloques que
 * el sombreado no ve nunca, asi que una cuenta enganchada solo al sombreado se
 * calla justo donde la comprobacion es mas estricta, y se calla SIN decirlo.
 *
 * Ninguno de los dos identificadores puede ser cero para que el par signifique
 * algo: cero es lo que contesta el deposito cuando una pila no cupo, y emparejar
 * un sitio real con "no se" pondria en la tabla una fila con aspecto de
 * conocimiento.  Esas se dejan fuera, y el deposito ya cuenta su propio
 * desbordamiento.  \~
 *
 * @param alloc_stack \~english who handed it out.  \~spanish quien lo entrego.
 *                    \~
 * @param free_stack  \~english who gave it back.  \~spanish quien lo devolvio.
 *                    \~
 * @param req         \~english the bytes that were asked for.  \~spanish los
 *                    bytes que se pidieron.  \~
 */
void note_pair(uint32_t alloc_stack, uint32_t free_stack,
               uint32_t req) noexcept {
    if (g_pairs == nullptr) return;
    if (alloc_stack == 0 || free_stack == 0) return;

    const uint64_t key =
        (uint64_t(alloc_stack) << 32) | uint64_t(free_stack);
    uint64_t v = key * 0x9E3779B97F4A7C15ull;
    uint32_t i = uint32_t(v >> (64 - kPairBits)) & (kPairSlots - 1);
    for (uint32_t probe = 0; probe < kPairProbe; ++probe) {
        Pair &p = g_pairs[i];
        uint64_t cur = p.key.load(std::memory_order_acquire);
        if (cur == 0) {
            uint64_t expected = 0;
            if (!p.key.compare_exchange_strong(expected, key,
                                               std::memory_order_acq_rel,
                                               std::memory_order_relaxed))
                cur = expected; // somebody got there first; see what they put
            else
                cur = key;
        }
        if (cur == key) {
            /* \~english Relaxed and not atomic-per-field: two threads releasing
             * the same pair at the same instant can lose a count.  That is
             * accepted here and it is a different thing from the table being
             * full -- what this answers is the SHAPE of the ownership, and a
             * missed unit does not turn one owner into several.  Making it
             * exact would put a locked instruction on every release for a digit
             * nobody reads.
             *
             * \~spanish Relajado y no atomico por campo: dos hilos soltando el
             * mismo par en el mismo instante pueden perder una cuenta.  Se
             * acepta, y es cosa distinta de que la tabla este llena -- lo que
             * esto contesta es la FORMA de la propiedad, y una unidad perdida no
             * convierte un dueño en varios.  Hacerlo exacto pondria una
             * instruccion con cerrojo en cada liberacion por un digito que no
             * lee nadie.  \~ */
            p.blocks += 1;
            p.bytes += req;
            return;
        }
        i = (i + 1) & (kPairSlots - 1);
    }
    g_pair_full.fetch_add(1, std::memory_order_relaxed);
}

// =========================================================================
//  Pages of its own, with a guard behind them
// =========================================================================

/**
 * @brief
 * \~english What is known about a block that lives on pages of its own.
 * \~spanish Lo que se sabe de un bloque que vive en paginas propias.
 * \~
 *
 * \~english
 * A SIDE TABLE and not a header in front of the block, and the reason is the
 * whole trick: the block is placed FLUSH against the guard page, so there is no
 * room behind it, and what is in front of it is a variable amount of slack that
 * cannot be found again from the pointer alone.
 *
 * \~spanish
 * UNA TABLA APARTE y no una cabecera delante del bloque, y la razon es el truco
 * entero: el bloque se coloca PEGADO a la pagina de guarda, asi que detras no
 * queda sitio, y lo que hay delante es una cantidad variable de hueco que no se
 * puede volver a encontrar solo con el puntero.
 * \~
 */
struct Guarded {
    std::atomic<const void *> p; ///< what the caller holds; nullptr = free slot
    const void *base;            ///< the first page of the reservation
    size_t total;                ///< bytes reserved, guard page included
    size_t req;                  ///< what the caller asked for
    size_t tail;                 ///< bytes between the block and the guard
    uint32_t alloc_stack;
    uint32_t free_stack;
    uint32_t state;
    /* \~english WHAT A LIFE IS MEASURED AGAINST, and it has to live here because
     * a guarded block has no shadow slot to keep it in.  Without the two, the
     * strictest level was the one where nothing had a measurable life: the
     * report said every site was instant, and the count of the longest life
     * stayed at zero -- the checker's own telemetry going blind at its own
     * strictest setting, and going blind WITHOUT saying so, which is the exact
     * failure this library exists to make impossible.
     *
     * \~spanish CONTRA QUE SE MIDE UNA VIDA, y tiene que vivir aqui porque un
     * bloque con guarda no tiene ranura de sombreado donde guardarlo.  Sin los
     * dos, el nivel mas estricto era aquel en el que nada tenia vida medible:
     * el informe decia que todo sitio era instantaneo, y la cuenta de la vida
     * mas larga se quedaba a cero -- la telemetria del propio comprobador
     * quedandose ciega en su ajuste mas estricto, y quedandose ciega SIN
     * decirlo, que es justo el fallo que esta libreria existe para impedir.  \~
     */
    uint32_t seq;    ///< \~english its thread's count at birth.  \~spanish la
                     ///< cuenta de su hilo al nacer.  \~
    uint32_t thread; ///< \~english who allocated it.  \~spanish quien lo
                     ///< reservo.  \~
};

/// \~english Open addressing, never evicting.  Full is COUNTED and the level
///           falls back for that allocation, which is loud; evicting would lose
///           a block's identity and turn a use-after-free into a wrong
///           accusation.
/// \~spanish Direccionamiento abierto, sin desalojar nunca.  Lleno se CUENTA y
///           el nivel cae al de siempre para esa reserva, que es ruidoso;
///           desalojar perderia la identidad de un bloque y convertiria un uso
///           despues de liberar en una acusacion equivocada.
/// \~
constexpr uint32_t kGuardSlots = 1u << 16;
Guarded *g_guarded = nullptr;

/**
 * @brief
 * \~english Slots in the index that answers for an address INSIDE a block.
 * \~spanish Ranuras del indice que contesta por una direccion DE DENTRO de un
 *           bloque.
 * \~
 *
 * \~english
 * WHY A SECOND TABLE AND NOT A WIDER FIRST ONE.  The table above is keyed by
 * the address handed out, which is the only thing a release or a size question
 * normally carries.  But the allocator promises more than that for a block it
 * got from the system: `direct_bytes` and `direct_take` answer for ANY address
 * inside one, so `host_free(p + 4096)` releases the whole block and
 * `host_usable_size(p + 4096)` counts what is left from there.
 *
 * A checking mode that broke that promise would not be catching a bug, it would
 * be inventing one: correct code would stop the process with a panic about a
 * block the allocator itself had served, and only when the mode was on.  A
 * checker that accuses correct code is worse than no checker.
 *
 * So the address is turned into a GRANULE and looked up here.  The granule is
 * @c os_reserve_granularity, and using anything else -- a written-down 4096, a
 * written-down 64 KiB -- would break it: that value is exactly the alignment
 * `os_reserve` hands back, which is what makes two facts true at once.  Every
 * guarded block starts ON a granule, so masking an interior address reaches its
 * first one in a single step; and no two reservations SHARE a granule, so one
 * granule names one block and the index is a function, with no lists to walk
 * and no collisions to resolve.  It is queried at run time in both systems --
 * 64 KiB on Windows, asked of ntdll rather than assumed, and the page size on
 * POSIX, which is 4096 on x86-64 and is NOT on several others.
 *
 * Sized for the whole of what the level can hold: @c kGuardSlots blocks, each
 * of which may cover more than one granule where the granule is the page.  Full
 * behaves like the table above -- the block is served without an entry and the
 * refusal is counted -- because a silent hole in an index that exists to avoid
 * a false accusation would be the false accusation coming back.
 *
 * \~spanish
 * POR QUE UNA SEGUNDA TABLA Y NO UNA PRIMERA MAS ANCHA.  La tabla de arriba se
 * indexa por la direccion entregada, que es lo unico que normalmente lleva una
 * liberacion o una pregunta de tamano.  Pero el asignador promete mas que eso
 * para un bloque que le dio el sistema: `direct_bytes` y `direct_take`
 * contestan por CUALQUIER direccion de dentro, asi que `host_free(p + 4096)`
 * suelta el bloque entero y `host_usable_size(p + 4096)` cuenta lo que queda
 * desde ahi.
 *
 * Un modo de comprobacion que rompiera esa promesa no estaria cazando un fallo,
 * estaria inventandolo: codigo correcto pararia el proceso con un panico sobre
 * un bloque que habia servido el propio asignador, y solo con el modo puesto.
 * Un comprobador que acusa a codigo correcto es peor que no tener comprobador.
 *
 * Asi que la direccion se convierte en un GRANULO y se busca aqui.  El granulo
 * es @c os_reserve_granularity, y usar otra cosa -- un 4096 escrito a mano, un
 * 64 KiB escrito a mano -- lo rompe: ese valor es exactamente la alineacion que
 * devuelve `os_reserve`, que es lo que hace ciertas dos cosas a la vez.  Todo
 * bloque con guarda empieza EN un granulo, asi que enmascarar una direccion
 * interior alcanza el primero de un solo paso; y dos reservas nunca COMPARTEN
 * granulo, asi que un granulo nombra un bloque y el indice es una funcion, sin
 * listas que recorrer ni colisiones que resolver.  Se pregunta en ejecucion en
 * los dos sistemas -- 64 KiB en Windows, pedidos a ntdll en vez de supuestos, y
 * el tamano de pagina en POSIX, que es 4096 en x86-64 y NO lo es en varios
 * otros.
 *
 * Dimensionado para todo lo que el nivel puede guardar: @c kGuardSlots bloques,
 * cada uno de los cuales puede cubrir mas de un granulo alli donde el granulo
 * es la pagina.  Lleno se comporta como la tabla de arriba -- el bloque se
 * sirve sin ficha y la negativa se cuenta -- porque un hueco callado en un
 * indice que existe para evitar una acusacion falsa seria la acusacion falsa
 * volviendo.  \~
 */
constexpr uint32_t kIndexSlots = 1u << 18;

/// \~english Granule -> entry, as slot number PLUS ONE so that zero is empty.
/// \~spanish Granulo -> ficha, como numero de ranura MAS UNO para que el cero
///           sea vacio.  \~
std::atomic<uint32_t> *g_gindex = nullptr;

/// \~english Granules an address is divided by; @c os_reserve_granularity.
/// \~spanish Granulos en que se divide una direccion; @c
///           os_reserve_granularity.  \~
size_t g_granule = 0;

/// \~english Interior lookups that found no room to be answerable.
/// \~spanish Busquedas interiores que no encontraron sitio para poder
///           contestarse.  \~
std::atomic<uint64_t> g_index_full{0};
std::atomic<uint64_t> g_guard_full{0};
std::atomic<uint64_t> g_guard_bytes{0};

/**
 * @brief
 * \~english Blocks this mode served ITSELF, out of its own pages.
 * \~spanish Bloques que este modo sirvio EL, de sus propias paginas.
 * \~
 *
 * \~english
 * WHY THE CHECKER COUNTS ITS OWN INSTEAD OF ADDING TO THE ALLOCATOR'S.  Because
 * the two numbers do not mean the same thing and making them agree would throw
 * one of them away.  `served` means "blocks this allocator carved out of a
 * region"; a guarded block was not carved out of anything -- the pages came
 * from the system and the allocator never saw it.  Adding to `served` would
 * turn it into "blocks that happened", which is what the site table already
 * counts, and the fact that most of a run came out of guard pages -- so its
 * memory behaviour is NOT the program's usual one -- would stop being visible.
 *
 * WHAT IT BUYS is an identity instead of an approximation:
 *
 *     site entries  ==  allocator served  +  checker served
 *
 * That has to hold at EVERY level, and below the guard one this counter is zero
 * so it degenerates into the equality the allocator already checked.  A test
 * that was red here by construction becomes a test of something.
 *
 * The bytes and the refusals were already counted on this path; only the blocks
 * were not.  It is one relaxed add, next to a call that has just asked the
 * system to reserve and commit pages.
 *
 * \~spanish
 * POR QUE EL COMPROBADOR CUENTA LOS SUYOS EN VEZ DE SUMARLOS A LOS DEL
 * ASIGNADOR.  Porque los dos numeros no significan lo mismo y hacerlos cuadrar
 * tiraria uno de los dos.  `served` significa "bloques que este asignador
 * recorto de una region"; un bloque con guarda no se recorto de nada -- las
 * paginas vinieron del sistema y el asignador no lo vio nunca.  Sumarlo a
 * `served` lo convertiria en "bloques que ocurrieron", que es lo que ya cuenta
 * la tabla de sitios, y dejaria de verse que la mayor parte de una corrida
 * salio de paginas de guarda -- o sea que su comportamiento de memoria NO es el
 * habitual del programa.
 *
 * LO QUE COMPRA es una identidad en vez de una aproximacion:
 *
 *     entradas de sitio  ==  servidas por el asignador  +  servidas por el
 *                            comprobador
 *
 * Y eso tiene que cumplirse en TODOS los niveles: por debajo del de guarda este
 * contador vale cero y degenera en la igualdad que el asignador ya comprobaba.
 * Un test que aqui salia rojo por construccion pasa a comprobar algo.
 *
 * Los bytes y los rechazos ya se contaban en este camino; los bloques no.  Es
 * una suma relajada, al lado de una llamada que acaba de pedirle al sistema que
 * reserve y comprometa paginas.
 * \~
 */
std::atomic<uint64_t> g_guard_blocks{0};

/**
 * @brief
 * \~english The other half: blocks this mode RELEASED itself.
 * \~spanish La otra mitad: bloques que este modo SOLTO el.
 * \~
 *
 * \~english
 * FOR THE SAME REASON AS THE COUNT ABOVE, and it is only half a count without
 * it.  A release that this level handles never reaches the allocator's lists,
 * so `large_frees` does not move for it -- and a test asking "were the four
 * hundred releases counted?" was reading one of the two ledgers and calling the
 * difference a failure.  With this the question has an answer that holds at
 * every level: served here plus served there, released here plus released
 * there.  Switched off it is zero and the sum is the allocator's own figure,
 * unchanged.
 *
 * \~spanish
 * POR LA MISMA RAZON QUE LA CUENTA DE ARRIBA, y sin el aquella es media cuenta.
 * Una liberacion que atiende este nivel no llega nunca a las listas del
 * asignador, asi que `large_frees` no se mueve por ella -- y un test que
 * preguntaba "se contaron las cuatrocientas devoluciones?" estaba leyendo uno
 * de los dos libros y llamando fallo a la diferencia.  Con esto la pregunta
 * tiene respuesta en todos los niveles: servidos aqui mas servidos alli,
 * soltados aqui mas soltados alli.  Apagado vale cero y la suma es la cifra del
 * asignador de siempre.  \~
 */
std::atomic<uint64_t> g_guard_frees{0};

/**
 * @brief
 * \~english The same count, split by PURPOSE, exactly as the allocator splits
 *           its own.
 * \~spanish La misma cuenta, repartida por PROPOSITO, igual que reparte el
 *           asignador la suya.
 * \~
 *
 * \~english
 * WHY IT CANNOT GO INTO THE ALLOCATOR'S TABLE, and here the data structure
 * decides it rather than a preference.  Its own comment says so where the
 * counting happens: "counting by tag IS counting: the total comes from summing
 * this table".  `by_tag` and `served` are the SAME number sliced by purpose, so
 * adding a guarded block to `by_tag` would add it to `served` through the back
 * door -- the very thing kept out one layer up, arrived at sideways.
 *
 * So the split lives here too, and the identity refines from a total into a
 * per-purpose one:
 *
 *     for each tag t:   entries(t)  ==  by_tag(t)  +  guard_by_tag(t)
 *
 * Summed over the sixteen it gives back the total identity, which stops being a
 * separate rule and becomes the consequence of these.
 *
 * IT ONLY WORKS BECAUSE THE THREAD HAS ITS CACHE.  The tag is read from it, and
 * until this mode started asking for the cache a guarded allocation had none --
 * so this would have counted everything as "not known" and looked like it
 * worked.  See the `ensure_cache` call in `san_alloc_guarded`.
 *
 * \~spanish
 * POR QUE NO PUEDE IR A LA TABLA DEL ASIGNADOR, y aqui lo decide la estructura
 * de datos y no una preferencia.  Su propio comentario lo dice donde se cuenta:
 * "contar por etiqueta ES contar: el total sale de sumar esta tabla".  `by_tag`
 * y `served` son el MISMO numero troceado por proposito, asi que sumar ahi un
 * bloque con guarda seria sumarlo a `served` por la puerta de atras -- justo lo
 * que se dejo fuera una capa mas arriba, entrando de lado.
 *
 * Asi que el reparto vive tambien aqui, y la identidad se refina de un total a
 * un reparto:
 *
 *     por cada etiqueta t:   entradas(t)  ==  by_tag(t)  +  guard_by_tag(t)
 *
 * Sumada sobre las dieciseis devuelve la identidad del total, que deja de ser
 * una regla aparte y pasa a ser la consecuencia de estas.
 *
 * SOLO FUNCIONA PORQUE EL HILO TIENE SU CACHE.  La etiqueta se lee de ella, y
 * hasta que este modo empezo a pedirla una reserva con guarda no tenia ninguna
 * -- asi que esto habria contado todo como "no se" y habria parecido que
 * funcionaba.  Ver la llamada a `ensure_cache` en `san_alloc_guarded`.
 * \~
 */
std::atomic<uint64_t> g_guard_by_tag[VESTA_ALLOC_TAG_SLOTS];

/**
 * @brief
 * \~english How far from its own slot an entry may be, looking and placing.
 * \~spanish A que distancia de su ranura puede estar una entrada, al buscar y
 *           al colocar.
 * \~
 *
 * \~english
 * IT HAS TO BE BOUNDED, and the number matters less than that it is the SAME
 * for both.  Slots here are never given back -- a released block keeps its
 * range so its address is handed to nobody else, which is what makes a
 * use-after-free point at the right code -- so on a real workload the table
 * saturates: 498.554 refusals and 6,4 GB held, measured.  With an unbounded
 * scan, saturation turns every placement AND every lookup into 65.536 probes,
 * and a program that allocates in millions simply stops: `test_host_allocator`
 * hung for minutes inside `guarded_free`, found by attaching a debugger to it.
 *
 * Bounded, both cost eight probes and the invariant that makes it correct is
 * one line: an entry is within the window of its own slot, or it was never
 * placed -- and a placement that did not fit already falls back to an ordinary
 * block, counted and printed.  The same shape and the same reason as
 * `kMaxProbe` in the allocation-sites table.
 *
 * \~spanish
 * TIENE QUE ESTAR ACOTADA, y el numero importa menos que el hecho de que sea el
 * MISMO para las dos.  Aqui las ranuras no se devuelven nunca -- un bloque
 * soltado conserva su rango para que su direccion no se le entregue a nadie
 * mas, que es lo que hace que un uso-tras-liberar apunte al codigo correcto --,
 * asi que en una carga de verdad la tabla se satura: 498.554 rechazos y 6,4 GB
 * retenidos, medido.  Con un barrido sin tope, saturarse convierte cada
 * colocacion Y cada busqueda en 65.536 sondeos, y un programa que reserva por
 * millones sencillamente se para: `test_host_allocator` se quedo colgado
 * minutos dentro de `guarded_free`, encontrado adjuntandole un depurador.
 *
 * Acotadas, las dos cuestan ocho sondeos y el invariante que lo hace correcto
 * cabe en una linea: una entrada esta dentro de la ventana de su propia
 * ranura, o no se coloco nunca -- y una colocacion que no cupo ya recurre a un
 * bloque normal, contada e impresa.  La misma forma y el mismo motivo que
 * `kMaxProbe` en la tabla de sitios de reserva.
 * \~
 */
constexpr uint32_t kGuardProbe = 8;

/**
 * @brief
 * \~english How many refusals in a row are taken as "this table is done".
 * \~spanish Cuantas negativas seguidas se toman como "esta tabla esta acabada".
 * \~
 *
 * \~english
 * AN ENTRY IS NEVER TAKEN BACK.  `guarded_free` decommits the pages and leaves
 * the entry standing -- that is what lets a later touch be named as a use after
 * free instead of being confused with whoever got the address next -- so the
 * occupancy of this table only ever goes UP.  Which means a refusal is not a
 * passing condition: once the windows fill, they stay full for the rest of the
 * run.
 *
 * That is worth a latch because of what a refusal COSTS.  The block's address
 * is what keys the entry, so it cannot be known before the mapping exists:
 * `san_alloc_guarded` has to reserve and commit pages from the system, ask, and
 * -- when the answer is no -- hand them straight back.  Three trips into the
 * kernel to learn something a counter already knew.
 *
 * Measured, on this library's own `host_allocator` test at this level: 54.560
 * blocks got an entry and 4.080.461 did not, and those refusals spent 101 of
 * the run's 120 seconds inside the kernel, all of it under `os_free` reached
 * from the ALLOCATION path.  With the latch the same run pays that toll @c
 * kGuardGiveUp times instead of four million.
 *
 * Eight and not one: a single refusal says nothing, since a window can be full
 * while the table is not.  Eight in a row say the neighbourhood is done.  And
 * it must sit well under the natural gap between placements -- measured at one
 * placement every 74 refusals, 55.308 against 4.07 million -- or the counter is
 * cleared before it ever arrives and the step back never happens: at 64 this
 * run still took 81 s, at 8 it takes 17 s.  See @c kGuardRetry.
 *
 * \~spanish
 * UNA FICHA NO SE RECUPERA NUNCA.  `guarded_free` descompromete las paginas y
 * deja la ficha puesta -- que es lo que permite que un toque posterior se
 * llame uso despues de liberar en vez de confundirse con quien cogiera la
 * direccion despues --, asi que la ocupacion de esta tabla solo SUBE.  Lo que
 * significa que una negativa no es una condicion pasajera: cuando las ventanas
 * se llenan, siguen llenas el resto de la corrida.
 *
 * Eso merece un pestillo por lo que CUESTA una negativa.  La direccion del
 * bloque es la clave de la ficha, asi que no se puede saber antes de que exista
 * el mapeo: `san_alloc_guarded` tiene que reservar y comprometer paginas del
 * sistema, preguntar y -- cuando la respuesta es no -- devolverlas tal cual.
 * Tres viajes al nucleo para enterarse de algo que un contador ya sabia.
 *
 * Medido, sobre el test `host_allocator` de esta misma libreria en este nivel:
 * 54.560 bloques consiguieron ficha y 4.080.461 no, y esas negativas se
 * llevaron 101 de los 120 segundos de la corrida dentro del nucleo, todos bajo
 * `os_free` alcanzado desde el camino de RESERVA.  Con el pestillo la misma
 * corrida paga ese peaje @c kGuardGiveUp veces en vez de cuatro millones.
 *
 * Ocho y no una: una negativa suelta no dice nada, porque una ventana puede
 * estar llena sin que lo este la tabla.  Ocho seguidas dicen que el barrio esta
 * acabado.  Y tiene que quedar MUY por debajo de la distancia natural entre
 * colocaciones -- medida en una colocacion cada 74 negativas, 55.308 contra
 * 4,07 millones --, o el contador se pone a cero antes de llegar y el paso
 * atras no ocurre nunca: con 64 esta corrida seguia tardando 81 s, con 8 tarda
 * 17 s.  Ver @c kGuardRetry.  \~
 */
constexpr uint32_t kGuardGiveUp = 8;

/**
 * @brief
 * \~english How many allocations are waved through before trying again.
 * \~spanish Cuantas reservas pasan de largo antes de volver a intentarlo.
 * \~
 *
 * \~english
 * BECAUSE GIVING UP FOR GOOD COSTS COVERAGE, and that was measured: a permanent
 * latch took the same run from 54.560 guarded blocks down to 33.040.  A run of
 * refusals proves the windows that were TRIED are full; it does not prove every
 * window is, and the ones still open are worth an occasional ask.
 *
 * So the refusal is a step back, not a door closed: one allocation in @c
 * kGuardRetry pays the round trip to find out, and a single placement clears
 * the counter and puts the level back to normal.
 *
 * AND STEPPING BACK FURTHER BUYS COVERAGE, which is the opposite of what one
 * would expect from skipping and is the whole reason for the size of this
 * number.  A refused reservation hands its pages back, and the system returns
 * THE SAME address to the next caller -- the same key, the same full window,
 * refused again, for as long as the run lasts.  That is why 43 % of the table
 * being blocked showed up as 98,7 % of attempts failing: the attempts were all
 * the same doomed handful.  Waving allocations through lets everything else
 * move the map, so the eventual retry lands somewhere new.
 *
 * The whole curve, same test, same machine, window of 8:
 *
 *       no step back at all       115.886 ms      54.560 blocks
 *       give up 64 / retry 64      81.025 ms      55.702
 *       give up  8 / retry 64      17.673 ms      54.814
 *       give up  8 / retry 512      3.455 ms      54.178
 *       give up  8 / retry 4096     1.502 ms      56.229   <- here
 *       give up  8 / retry 32768    1.427 ms      55.439
 *       permanent latch               681 ms      33.040
 *
 * Flat past this point, so there is nothing left to buy: 4096 is where the
 * curve stops falling, and it covers MORE than the version that never stepped
 * back at all.
 *
 * \~spanish
 * PORQUE RENDIRSE DEL TODO CUESTA COBERTURA, y eso tambien esta medido: un
 * pestillo permanente llevo la misma corrida de 54.560 bloques con guarda a
 * 33.040.  Una racha de negativas demuestra que las ventanas que se PROBARON
 * estan llenas; no demuestra que lo esten todas, y las que sigan abiertas
 * merecen que se pregunte de vez en cuando.
 *
 * Asi que la negativa es un paso atras, no una puerta cerrada: una reserva de
 * cada @c kGuardRetry paga el viaje para averiguarlo, y una sola colocacion
 * pone el contador a cero y devuelve el nivel a la normalidad.
 *
 * Y RETIRARSE MAS COMPRA COBERTURA, que es lo contrario de lo que uno esperaria
 * de saltarse intentos y es la razon entera del tamano de este numero.  Una
 * reserva negada devuelve sus paginas, y el sistema le da LA MISMA direccion al
 * siguiente que pregunte -- la misma clave, la misma ventana llena, negada otra
 * vez, mientras dure la corrida.  Por eso un 43 % de la tabla bloqueada salia
 * como un 98,7 % de intentos fallidos: los intentos eran todos el mismo punado
 * de condenados.  Dejar pasar reservas permite que todo lo demas mueva el mapa,
 * asi que el reintento cae en otro sitio.
 *
 * La curva entera, mismo test, misma maquina, ventana de 8:
 *
 *       sin ningun paso atras     115.886 ms      54.560 bloques
 *       rendirse 64 / reintento 64 81.025 ms      55.702
 *       rendirse  8 / reintento 64 17.673 ms      54.814
 *       rendirse  8 / reint. 512    3.455 ms      54.178
 *       rendirse  8 / reint. 4096   1.502 ms      56.229   <- aqui
 *       rendirse  8 / reint. 32768  1.427 ms      55.439
 *       pestillo permanente           681 ms      33.040
 *
 * Plana a partir de aqui, asi que no queda nada que comprar: 4096 es donde la
 * curva deja de bajar, y cubre MAS que la version que no se retiraba nunca.  \~
 */
constexpr uint32_t kGuardRetry = 4096;

/// \~english Refusals in a row; reset by any placement.  \~spanish Negativas
/// seguidas; cualquier colocacion lo pone a cero.  \~
std::atomic<uint32_t> g_guard_misses{0};

/// \~english Allocations still to be waved through before asking again.
/// \~spanish Reservas que faltan por dejar pasar antes de volver a preguntar.
/// \~
std::atomic<uint32_t> g_guard_skip{0};

uint32_t guard_hash(const void *p) noexcept {
    uint64_t v = reinterpret_cast<uintptr_t>(p) >> 4;
    v *= 0x9E3779B97F4A7C15ull;
    return uint32_t(v >> 48) & (kGuardSlots - 1);
}

/// The entry for @p p, or nullptr.  Never inserts: looking up must not create.
Guarded *guard_find(const void *p) noexcept {
    if (g_guarded == nullptr) return nullptr;
    uint32_t i = guard_hash(p);
    /* \~english THE SAME WINDOW AS `guard_insert`, and that is the whole of
     * why this is correct: what could not be placed within it was never placed
     * at all, so not finding it here is the truth.  The old early-out -- a slot
     * never used -- stopped working the day the table filled, because a
     * released block keeps its slot forever and stops being a hole.  See
     * @c kGuardProbe.
     *
     * \~spanish LA MISMA VENTANA QUE `guard_insert`, y en eso consiste que esto
     * sea correcto: lo que no se pudo colocar dentro de ella no se coloco, asi
     * que no encontrarlo aqui es la verdad.  La salida temprana de antes -- una
     * ranura nunca usada -- dejo de funcionar el dia que la tabla se lleno,
     * porque un bloque soltado se queda con su ranura para siempre y deja de
     * ser un hueco.  Ver @c kGuardProbe.  \~ */
    for (uint32_t probe = 0; probe < kGuardProbe; ++probe) {
        Guarded &g = g_guarded[i];
        const void *cur = g.p.load(std::memory_order_acquire);
        if (cur == p) return &g;
        if (cur == nullptr && g.base == nullptr) return nullptr; // never used
        i = (i + 1) & (kGuardSlots - 1);
    }
    return nullptr;
}

Guarded *guard_insert(const void *p) noexcept {
    if (g_guarded == nullptr) return nullptr;
    uint32_t i = guard_hash(p);
    for (uint32_t probe = 0; probe < kGuardProbe; ++probe) {
        Guarded &g = g_guarded[i];
        const void *expected = nullptr;
        if (g.p.load(std::memory_order_relaxed) == nullptr &&
            g.p.compare_exchange_strong(expected, p, std::memory_order_acq_rel,
                                        std::memory_order_relaxed))
        {
            /* It placed one, so the table is not done after all. */
            g_guard_misses.store(0, std::memory_order_relaxed);
            return &g;
        }
        i = (i + 1) & (kGuardSlots - 1);
    }
    g_guard_full.fetch_add(1, std::memory_order_relaxed);
    if (g_guard_misses.fetch_add(1, std::memory_order_relaxed) + 1 >=
        kGuardGiveUp) {
        g_guard_misses.store(0, std::memory_order_relaxed);
        g_guard_skip.store(kGuardRetry, std::memory_order_relaxed);
    }
    return nullptr;
}

/// \~english The slot for a granule, by the same multiplicative spread.
/// \~spanish La ranura de un granulo, con el mismo reparto multiplicativo.  \~
uint32_t index_hash(uintptr_t granule) noexcept {
    uint64_t v = uint64_t(granule) * 0x9E3779B97F4A7C15ull;
    return uint32_t(v >> 46) & (kIndexSlots - 1);
}

/**
 * @brief
 * \~english Registers every granule @p base covers as belonging to @p slot.
 * \~spanish Apunta cada granulo que cubre @p base como perteneciente a @p slot.
 * \~
 *
 * \~english
 * EVERY GRANULE AND NOT JUST THE FIRST, because the alternative is walking
 * backwards from an interior address until a start turns up, and how far back
 * that is depends on the block -- unbounded in principle, thousands of steps
 * for a big block where the granule is a page.  Writing them all costs once,
 * at the only moment that already asked the system for pages; the lookup then
 * costs one hash whatever the block's size.
 *
 * \~spanish
 * CADA GRANULO Y NO SOLO EL PRIMERO, porque la alternativa es ir hacia atras
 * desde una direccion interior hasta dar con un comienzo, y cuanto hay que
 * retroceder depende del bloque -- ilimitado en principio, miles de pasos para
 * un bloque grande alli donde el granulo es la pagina.  Escribirlos todos se
 * paga una vez, en el unico momento que ya le habia pedido paginas al sistema;
 * la busqueda cuesta entonces un hash sea cual sea el tamano del bloque.  \~
 *
 * @return \~english false when a granule found no room, and then the block
 *         cannot be answered for from the inside.  \~spanish false cuando algun
 *         granulo no encontro sitio, y entonces no se puede contestar por el
 *         bloque desde dentro.  \~
 */
bool index_insert(const void *base, size_t total, uint32_t slot) noexcept {
    if (g_gindex == nullptr || g_granule == 0) return false;
    const uintptr_t a = reinterpret_cast<uintptr_t>(base);
    const uintptr_t first = a / g_granule;
    const uintptr_t last = (a + total - 1) / g_granule;
    for (uintptr_t g = first; g <= last; ++g) {
        uint32_t i = index_hash(g);
        bool placed = false;
        for (uint32_t probe = 0; probe < kGuardProbe; ++probe) {
            uint32_t expected = 0;
            if (g_gindex[i].load(std::memory_order_relaxed) == 0 &&
                g_gindex[i].compare_exchange_strong(
                    expected, slot + 1, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                placed = true;
                break;
            }
            i = (i + 1) & (kIndexSlots - 1);
        }
        if (!placed) {
            g_index_full.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }
    return true;
}

/**
 * @brief
 * \~english The block an address falls INSIDE, or nullptr.
 * \~spanish El bloque DENTRO del cual cae una direccion, o nullptr.
 * \~
 *
 * \~english
 * The granule names at most one block, so what comes back is either that block
 * or nothing.  Whether @p q is really inside it is then a comparison against
 * what the caller was promised -- the block, not the pages around it -- because
 * a granule covers the guard page and the slack as well, and answering for
 * those would be answering for memory nobody was given.
 *
 * \~spanish
 * El granulo nombra como mucho un bloque, asi que lo que vuelve es ese bloque o
 * nada.  Que @p q este de verdad dentro es entonces una comparacion contra lo
 * que se le prometio a quien llama -- el bloque, no las paginas de alrededor --
 * porque un granulo cubre tambien la pagina de guarda y la holgura, y contestar
 * por ellas seria contestar por memoria que no se entrego a nadie.  \~
 */
Guarded *guard_locate(const void *q) noexcept {
    if (g_gindex == nullptr || g_guarded == nullptr || g_granule == 0)
        return nullptr;
    const uintptr_t a = reinterpret_cast<uintptr_t>(q);
    uint32_t i = index_hash(a / g_granule);
    for (uint32_t probe = 0; probe < kGuardProbe; ++probe) {
        const uint32_t v = g_gindex[i].load(std::memory_order_acquire);
        if (v == 0) return nullptr; // never used: nothing is further along
        Guarded &g = g_guarded[v - 1];
        const uintptr_t start =
            reinterpret_cast<uintptr_t>(g.p.load(std::memory_order_acquire));
        if (start != 0 && a >= start && a - start < g.req) return &g;
        i = (i + 1) & (kIndexSlots - 1);
    }
    return nullptr;
}

// =========================================================================
//  Saying it
// =========================================================================

/**
 * @brief
 * \~english One frame, resolved by asking WHOSE the address is FIRST.
 * \~spanish Un marco, resuelto preguntando PRIMERO de quien es la direccion.
 * \~
 *
 * \~english
 * That order is the whole point, and it is a trap this library has already
 * fallen into once: our own symbol table covers our own module and nothing
 * else, so handing it an address from `libstdc++` or from the loader makes it
 * answer with the last symbol it happens to have -- `_fini` -- and an offset of
 * some absurd size.  A name that is confidently wrong sends the reader to fix
 * the wrong function, which is worse than no name at all.
 *
 * So: whose is it?  If ours, resolve it.  If not, say which module and the
 * displacement inside it, which is the truth AND is the same between runs,
 * because the load address moves and the displacement does not.
 *
 * \~spanish
 * Ese orden es lo importante, y es una trampa en la que esta libreria ya cayo
 * una vez: nuestra tabla de simbolos cubre nuestro modulo y nada mas, asi que
 * darle una direccion de `libstdc++` o del cargador hace que conteste con el
 * ultimo simbolo que tenga -- `_fini` -- y un desplazamiento de tamano
 * absurdo.  Un nombre seguro de si mismo y equivocado manda a arreglar otra
 * funcion, que es peor que no dar nombre.
 *
 * Asi que: de quien es?  Si es nuestra, se resuelve.  Si no, se dice de que
 * modulo y el desplazamiento dentro de el, que es la verdad Y es lo mismo entre
 * corridas, porque la direccion de carga se mueve y el desplazamiento no.
 * \~
 */
/// How many inline levels are asked for at one address.  A chain deeper than
/// this is cut and said, never silently shortened.
constexpr unsigned kNameFrames = 16;

/**
 * @brief
 * \~english The best name this process can give an address.
 * \~spanish El mejor nombre que este proceso puede dar a una direccion.
 * \~
 *
 * \~english
 * ONE PLACE, because there are two readers -- the text report and the tables --
 * and the whole point of the resolver existing is that they cannot disagree.
 * They already did: the terminal printed bare offsets while the CSV of the same
 * process printed real names, which reads as two different measurements.
 *
 * Three qualities, in order, and which one came out is visible in the answer
 * rather than guessed: a resolver installed by whoever links this library gives
 * function, file, line and the inline chain; failing that, a foreign module
 * gives its name and an offset; failing that, our own symbol table gives a
 * name; failing that, the address, which is still true.
 *
 * \~spanish
 * UN SOLO SITIO, porque hay dos lectores -- el informe de texto y las tablas --
 * y de eso va justamente que exista el resolutor: que no puedan contradecirse.
 * Ya lo hicieron: la terminal imprimia desplazamientos pelados mientras el CSV
 * del mismo proceso imprimia nombres de verdad, que se lee como dos mediciones
 * distintas.
 *
 * Tres calidades, en orden, y cual salio se ve en la respuesta en vez de
 * adivinarse: un resolutor instalado por quien enlaza esta libreria da funcion,
 * fichero, linea y la cadena de inline; si no lo hay, un modulo ajeno da su
 * nombre y un desplazamiento; si no, nuestra tabla de simbolos da un nombre; y
 * si no, la direccion, que sigue siendo cierta.
 * \~
 *
 * @return
 * \~english how many frames were written, or 0 when no resolver knew it.
 * \~spanish cuantos marcos se escribieron, o 0 si ningun resolutor la conocia.
 * \~
 */
unsigned name_of(const void *pc, AllocFrame *out, unsigned max) noexcept {
    const AllocSymbolResolver r = alloc_symbol_resolver();
    if (r == nullptr) return 0;
    /* \~english Cleared BEFORE the call and not once at declaration: a resolver
     * fills in only what it knows, so a field it does not touch would keep what
     * the PREVIOUS address left there -- a wrong file that looks right, which
     * is worse than an empty one.
     *
     * \~spanish Se limpia ANTES de la llamada y no una sola vez al declararlo:
     * un resolutor solo rellena lo que sabe, asi que un campo que no toque
     * quedaria con lo que dejo la direccion ANTERIOR -- un fichero equivocado
     * que parece bueno, que es peor que uno vacio.  \~ */
    vesta_memfill(out, 0, max);
    return r(pc, out, max);
}

/* \~english What an address is when nobody could resolve it: a module and an
 * offset, or our own symbol, or the bare address.  Into @p buf so the two
 * readers format it their own way from the same text.
 *
 * \~spanish Lo que es una direccion cuando nadie la pudo resolver: un modulo y
 * un desplazamiento, o un simbolo nuestro, o la direccion pelada.  A @p buf,
 * para que los dos lectores le den su formato a partir del mismo texto.  \~ */
void raw_name(const void *pc, char *buf, size_t cap) noexcept {
    VestaModuleInfo m;
    if (vesta_module_of(pc, &m) && !vesta_module_is_self(pc)) {
        const char *path = m.path != nullptr ? m.path : "?";
        /* Just the file name: a full path buries the one part that matters in
         * a line of directories nobody reads. */
        for (const char *s = path; *s != '\0'; ++s)
            if (*s == '/' || *s == '\\') path = s + 1;
        std::snprintf(buf, cap, "%s +0x%zx", path, m.offset);
        return;
    }
    size_t off = 0;
    const char *name = self_symbol(pc, &off);
    if (name != nullptr)
        /* \~english Through the formatter, which is the half that was missing:
         * without it this printed the mangled name while the allocator's own
         * report, reading the SAME table, printed the readable one.
         *
         * \~spanish Por el formateador, que es la mitad que faltaba: sin el
         * esto imprimia el nombre decorado mientras el informe del propio
         * asignador, leyendo la MISMA tabla, imprimia el legible.  \~ */
        std::snprintf(buf, cap, "%s +0x%zx", alloc_readable_name(name), off);
    else
        std::snprintf(buf, cap, "%p", pc);
}

void print_frame(const void *pc) noexcept {
    AllocFrame fr[kNameFrames];
    const unsigned got = name_of(pc, fr, kNameFrames);
    if (got != 0) {
        for (unsigned k = 0; k < got; ++k) {
            const char *fn = alloc_readable_name(fr[k].function);
            std::fprintf(stderr, "      %s%s", fn != nullptr ? fn : "?",
                         fr[k].inlined ? "  [inlined]" : "");
            if (fr[k].file != nullptr)
                std::fprintf(stderr, "  (%s:%u)", fr[k].file, fr[k].line);
            std::fputc('\n', stderr);
        }
        return;
    }
    char buf[512];
    raw_name(pc, buf, sizeof buf);
    std::fprintf(stderr, "      %s\n", buf);
}

void print_stack(const char *what, uint32_t id) noexcept {
    /* \~english HERE AND NOT ONLY IN THE REPORT, because verdicts print stacks
     * too -- mid-run, every time something is caught -- and naming an address
     * reads DWARF, and reading DWARF allocates.  Measured at 41 MB through
     * `operator new` in one test run, landing in the program's own figures as
     * if the program had asked for them.  Putting the guard in the one function
     * every printer goes through covers the verdicts and the report at once,
     * and saving and restoring means a stack printed from inside the report
     * does not clear it on the way out.
     *
     * \~spanish AQUI Y NO SOLO EN EL INFORME, porque los veredictos tambien
     * imprimen pilas -- a mitad de corrida, cada vez que se caza algo -- y
     * nombrar una direccion lee DWARF, y leer DWARF reserva.  Medido en 41 MB
     * por `operator new` en una sola corrida del test, aterrizando en las
     * cifras del propio programa como si el programa las hubiera pedido.
     * Poner la guarda en la unica funcion por la que pasan todos los que
     * imprimen cubre los veredictos y el informe de una vez, y guardar y
     * restaurar hace que una pila impresa desde dentro del informe no lo
     * apague al salir.  \~ */
    const bool was_reporting = g_in_report;
    g_in_report = true;
    struct Restore {
        bool prev;
        ~Restore() { g_in_report = prev; }
    } restore{was_reporting};

    if (id == 0 || g_depot == nullptr) {
        std::fprintf(stderr, "    %s: not available\n", what);
        return;
    }
    const Stack &s = g_depot[id];
    std::fprintf(stderr, "    %s%s:\n", what,
                 s.walked ? "" : " (one frame: no frame pointer to follow)");
    for (unsigned i = 0; i < s.frames; ++i) print_frame(s.pc[i]);
}

/// Every verdict goes through here, so none of them can forget to be counted --
/// and the exit code is that count.
void verdict(Certainty c, const char *headline, const void *p) noexcept {
    g_verdicts.fetch_add(1, std::memory_order_relaxed);
    if (c == Certainty::Proven) g_proven.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "\n[allocator/check] %s: %s at %p\n",
                 certainty_word(c), headline, p);
}

// =========================================================================
//  The canary
// =========================================================================

/**
 * @brief
 * \~english The part of a released block the poison may touch.
 * \~spanish La parte de un bloque soltado que el veneno puede tocar.
 * \~
 *
 * \~english
 * IT DOES NOT START AT THE BEGINNING, and that is the allocator being right and
 * the checker adapting.  A block on a free list carries the link to the next
 * one IN ITS FIRST BYTES -- `push_block` writes it there, which is why freeing
 * is as cheap as it is -- so the allocator overwrites whatever the checker put
 * at offset zero, the instant after it put it.
 *
 * Poisoning them anyway is what the first version did, and it produced a flood
 * of "written after release" on every reused block: the checker accusing the
 * allocator of the very thing it was built to do.  A checker that cries wolf
 * gets switched off and never comes back, so the link goes untouched.
 *
 * \~spanish
 * NO EMPIEZA AL PRINCIPIO, y eso es el asignador teniendo razon y el
 * comprobador adaptandose.  Un bloque en una lista de libres lleva el enlace al
 * siguiente EN SUS PRIMEROS BYTES -- `push_block` lo escribe ahi, que es por lo
 * que liberar sale tan barato --, asi que el asignador pisa lo que el
 * comprobador pusiera en el desplazamiento cero, al instante siguiente de
 * ponerlo.
 *
 * Envenenarlos igual es lo que hizo la primera version, y salio un torrente de
 * "escrito despues de soltar" en cada bloque reutilizado: el comprobador
 * acusando al asignador de justo lo que fue construido para hacer.  Un
 * comprobador que grita en falso se apaga y no vuelve, asi que el enlace se
 * queda sin tocar.
 * \~
 *
 * @param block
 * \~english the size of the block, which is what bounds this: one byte past it
 *           would be the checker corrupting the NEXT block.
 * \~spanish el tamano del bloque, que es lo que lo acota: un byte mas alla
 *           seria el comprobador corrompiendo el bloque SIGUIENTE.
 * \~
 */
constexpr size_t kLinkBytes = sizeof(void *);

[[gnu::always_inline]] inline size_t poison_from() noexcept {
    return kLinkBytes;
}

[[gnu::always_inline]] inline size_t poison_len(size_t block) noexcept {
    if (block <= kLinkBytes) return 0;
    const size_t room = block - kLinkBytes;
    if (g_poison == SanPoison::Whole) return room;
    return room < 64 ? room : 64; // one cache line, or all there is
}

/// \~english Whether the guard bytes fit behind @p req inside a block of
///           @p block bytes.  Asked on both sides -- writing it and checking it
///           -- so neither has to remember what the other did.
/// \~spanish Si los bytes de guarda caben detras de @p req dentro de un bloque
///           de @p block bytes.  Se pregunta por los dos lados -- al escribirlo
///           y al comprobarlo -- para que ninguno tenga que recordar lo que
///           hizo el otro.
/// \~
[[gnu::always_inline]] inline bool canary_fits(size_t req,
                                               size_t block) noexcept {
    return req + kCanaryBytes <= block;
}

[[gnu::always_inline]] inline void write_canary(void *p, size_t req) noexcept {
    std::memset(static_cast<unsigned char *>(p) + req, kCanaryByte,
                kCanaryBytes);
}

/// @return how many of the canary bytes survived; @c kCanaryBytes means intact.
unsigned check_canary(const void *p, size_t req) noexcept {
    const unsigned char *c = static_cast<const unsigned char *>(p) + req;
    unsigned intact = 0;
    while (intact < kCanaryBytes && c[intact] == kCanaryByte) ++intact;
    return intact;
}

// =========================================================================
//  Start-up
// =========================================================================

/// \~english A number from the environment, clamped.  Clamped and not rejected
///           because a typo in a knob must not silently switch the checker off:
///           asking for level 9 gets the highest there is, which is what the
///           person meant.
/// \~spanish Un numero del entorno, acotado.  Acotado y no rechazado porque una
///           errata en un ajuste no puede apagar el comprobador en silencio:
///           pedir el nivel 9 da el mas alto que haya, que es lo que la persona
///           queria decir.
/// \~
unsigned env_num(const char *name, unsigned def, unsigned max) noexcept {
    char buf[16];
    const size_t n = os_env(name, buf, sizeof buf);
    if (n == 0 || n >= sizeof buf) return def;
    unsigned v = 0;
    bool any = false;
    for (const char *s = buf; *s >= '0' && *s <= '9'; ++s) {
        v = v * 10 + unsigned(*s - '0');
        any = true;
    }
    if (!any) return def;
    return v > max ? max : v;
}

/**
 * @brief
 * \~english Sets the checker up, and says whether it can work yet.
 * \~spanish Monta el comprobador, y dice si ya puede trabajar.
 * \~
 *
 * \~english
 * TWO STEPS AND NOT ONE, and the reason cost a debugging round: the knobs can
 * be read at any time, but the shadow can only be sized once the ALLOCATOR'S
 * region exists -- and on the very first allocation of the process it does not,
 * because that allocation is what creates it.  A single step that latched
 * itself as "done" on that first call left the shadow at nothing FOREVER, and
 * the checker then reported a clean run on a program full of deliberate bugs.
 *
 * So the knobs latch and the shadow retries.  Not a constructor either: by then
 * the allocator is already answering, and a constructor runs at a moment nobody
 * chose.
 *
 * \~spanish
 * DOS PASOS Y NO UNO, y la razon costo una vuelta de depuracion: los ajustes se
 * pueden leer en cualquier momento, pero el sombreado solo se puede dimensionar
 * cuando existe la region DEL ASIGNADOR -- y en la primerisima reserva del
 * proceso no existe, porque esa reserva es la que la crea --.  Un solo paso que
 * se marcara "hecho" en esa primera llamada dejaba el sombreado en nada PARA
 * SIEMPRE, y entonces el comprobador daba una corrida limpia sobre un programa
 * lleno de fallos a proposito.
 *
 * Asi que los ajustes se fijan y el sombreado REINTENTA.  Tampoco desde un
 * constructor: para entonces el asignador ya esta contestando, y un constructor
 * corre en un momento que no eligio nadie.
 * \~
 */
/**
 * @brief
 * \~english The knobs, the depot and the guarded table.  Everything but the
 *           shadow.
 * \~spanish Los ajustes, el deposito y la tabla de bloques con guarda.  Todo
 *           menos el sombreado.
 * \~
 *
 * \~english
 * SEPARATE FROM THE SHADOW because they become usable at different moments, and
 * tying them together made the guard level never run: the shadow needs the
 * allocator's region, which on the very first allocation of the process does
 * not exist yet -- and a guarded block does not need the shadow at all, since
 * it lives on pages of its own with its own table.  Asking for both meant the
 * first allocation was never guarded, and in a program whose first allocation
 * is the one being tested, that means NONE of them were.
 *
 * \~spanish
 * SEPARADO DEL SOMBREADO porque se vuelven usables en momentos distintos, y
 * atarlos hizo que el nivel de guarda no corriera nunca: el sombreado necesita
 * la region del asignador, que en la primerisima reserva del proceso todavia no
 * existe -- y un bloque con guarda no usa el sombreado para nada, porque vive
 * en paginas propias con su propia tabla --.  Pedir los dos significaba que la
 * primera reserva nunca iba guardada, y en un programa cuya primera reserva es
 * justo la que se esta probando, eso son TODAS.
 * \~
 */
bool ensure_config() noexcept;

bool ensure_ready() noexcept {
    if (!ensure_config()) return false;
    if (detail::g_san_level == SanLevel::Off) return false;
    if (g_rows != nullptr) return true;

    /* \~english Sized by what the region ACTUALLY reserved, never by
     * `kRegionBytes`: the region asks for a maximum and takes what the system
     * gives, so the constant would size this for memory that may not exist.
     *
     * \~spanish Dimensionado por lo que la region reservo DE VERDAD, nunca por
     * `kRegionBytes`: la region pide un maximo y se queda con lo que le da el
     * sistema, asi que la constante lo dimensionaria para memoria que puede no
     * existir.  \~ */
    const size_t reserved = host_region_reserved();
    if (reserved == 0) return false; // no region yet: try again next time

    static std::atomic<int> rows_state{0};
    int rst = 0;
    if (!rows_state.compare_exchange_strong(rst, 1, std::memory_order_acq_rel,
                                            std::memory_order_acquire))
        return g_rows != nullptr;

    const uint32_t count = uint32_t(reserved / kChunkBytes);
    const size_t bytes = size_t(count) * sizeof(std::atomic<Slot *>);
    void *rows = os_alloc(bytes, kOsReadWrite);
    if (rows == nullptr) {
        rows_state.store(0, std::memory_order_release); // let it be retried
        return false;
    }
    std::memset(rows, 0, bytes);
    g_row_count = count;
    g_rows = static_cast<std::atomic<Slot *> *>(rows);
    rows_state.store(2, std::memory_order_release);
    return true;
}

bool ensure_config() noexcept {
    static std::atomic<int> cfg{0}; // 0 = untouched, 1 = doing it, 2 = done
    int st = cfg.load(std::memory_order_acquire);
    if (st != 2) {
        int expected = 0;
        if (cfg.compare_exchange_strong(expected, 1, std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
            detail::g_san_level =
                SanLevel(env_num("VESTA_ALLOC_SAN", unsigned(SanLevel::Track),
                                 unsigned(SanLevel::Guard)));
            g_poison = SanPoison(env_num("VESTA_ALLOC_SAN_POISON",
                                         unsigned(SanPoison::Line),
                                         unsigned(SanPoison::Whole)));
            /* \~english Clamped to what a depot entry can hold: asking for more
             * than `kFrames` would write past the end of one, and the checker
             * corrupting memory is the one thing it may never do.
             *
             * \~spanish Acotado a lo que cabe en una entrada del deposito:
             * pedir mas de `kFrames` escribiria pasado su final, y que el
             * comprobador corrompa memoria es lo unico que no puede hacer
             * jamas.  \~ */
            g_depth = env_num("VESTA_ALLOC_SAN_DEPTH", 2, kFrames);
            if (g_depth == 0) g_depth = 1;
            g_guard_edge = SanGuard(env_num("VESTA_ALLOC_SAN_GUARD",
                                            unsigned(SanGuard::Overflow),
                                            unsigned(SanGuard::Underflow)));
            g_exit_code = env_num("VESTA_ALLOC_SAN_EXITCODE", 1, 2);

            /* \~english The table for the guarded blocks, and only when that
             * level was asked for: it is two and a half megabytes, and a level
             * that is not in force should not cost them.
             *
             * \~spanish La tabla de los bloques con guarda, y solo cuando se ha
             * pedido ese nivel: son dos megas y medio, y un nivel que no esta
             * en vigor no puede costarlos.  \~ */
            if (detail::g_san_level >= SanLevel::Guard) {
                void *t = os_alloc(sizeof(Guarded) * kGuardSlots, kOsReadWrite);
                if (t != nullptr) {
                    std::memset(t, 0, sizeof(Guarded) * kGuardSlots);
                    g_guarded = static_cast<Guarded *>(t);
                    /* \~english And the index that answers from INSIDE a block.
                     * The granule is read from the system here, once: it is the
                     * alignment `os_reserve` gives back, and every guarded block
                     * therefore starts on one.  Failing to get the index is not
                     * failing the level -- blocks are still guarded, only an
                     * interior address cannot be placed -- so it drops nothing.
                     *
                     * \~spanish Y el indice que contesta desde DENTRO de un
                     * bloque.  El granulo se le pregunta al sistema aqui, una
                     * vez: es la alineacion que devuelve `os_reserve`, y por eso
                     * todo bloque con guarda empieza en uno.  No conseguir el
                     * indice no es fallar el nivel -- los bloques se siguen
                     * guardando, solo que una direccion interior no se puede
                     * situar --, asi que no baja nada.  \~ */
                    g_granule = os_reserve_granularity();
                    void *x = os_alloc(
                        sizeof(std::atomic<uint32_t>) * kIndexSlots,
                        kOsReadWrite);
                    if (x != nullptr) {
                        std::memset(x, 0,
                                    sizeof(std::atomic<uint32_t>) * kIndexSlots);
                        g_gindex = static_cast<std::atomic<uint32_t> *>(x);
                    } else {
                        std::fprintf(
                            stderr,
                            "[allocator/check] no room for the index of "
                            "guarded blocks: an address INSIDE one cannot be "
                            "placed, so releasing a block from the middle -- "
                            "which the allocator allows -- will be reported as "
                            "a foreign pointer\n");
                    }
                } else {
                    std::fprintf(stderr,
                                 "[allocator/check] no room for the table of "
                                 "guarded blocks: dropping to level %u, which "
                                 "catches an overflow when the block is "
                                 "released and not where it happens\n",
                                 unsigned(SanLevel::Poison));
                    detail::g_san_level = SanLevel::Poison;
                }
            }
            g_life_instant = env_num("VESTA_ALLOC_SAN_INSTANT", 100, 1u << 30);
            g_life_medium =
                env_num("VESTA_ALLOC_SAN_MEDIUM", 100000, 1u << 30);

            void *depot = os_alloc(sizeof(Stack) * kDepotSlots, kOsReadWrite);
            if (depot != nullptr) {
                std::memset(depot, 0, sizeof(Stack) * kDepotSlots);
                g_depot = static_cast<Stack *>(depot);
            }
            void *life = os_alloc(sizeof(Life) * kDepotSlots, kOsReadWrite);
            if (life != nullptr) {
                std::memset(life, 0, sizeof(Life) * kDepotSlots);
                g_life = static_cast<Life *>(life);
            }
            /* \~english And the pairs.  Not getting them is not failing the
             * level: everything else works, only the question of who OWNS a
             * site goes unanswered -- so it says so and carries on, instead of
             * dropping to a lower level over a table that catches nothing.
             *
             * \~spanish Y los pares.  No conseguirlos no es fallar el nivel:
             * todo lo demas funciona, solo se queda sin contestar la pregunta
             * de quien es DUEÑO de un sitio -- asi que lo dice y sigue, en vez
             * de bajar de nivel por una tabla que no caza nada.  \~ */
            void *pairs = os_alloc(sizeof(Pair) * kPairSlots, kOsReadWrite);
            if (pairs != nullptr) {
                std::memset(pairs, 0, sizeof(Pair) * kPairSlots);
                g_pairs = static_cast<Pair *>(pairs);
            } else {
                std::fprintf(stderr,
                             "[allocator/check] no room for the table of "
                             "site PAIRS: everything else still works, but "
                             "nothing will be able to say whether a site has "
                             "one owner or several\n");
            }
            cfg.store(2, std::memory_order_release);
        } else {
            return false; // somebody else is in there; sit this one out
        }
    }
    return true;
}

} // namespace

// =========================================================================
//  What the allocator calls
// =========================================================================

/* \~english THE SET-UP GOES FIRST, IN BOTH ENTRIES, and it is not a detail: the
 * level starts at `Off` and only becomes what the environment asked for inside
 * `ensure_ready`.  Testing the level before running it means the answer is
 * always `Off`, the checker never starts, and it reports a clean run on a
 * program full of mistakes -- which is the one failure mode a checker must not
 * have.  Found by pointing it at four deliberate bugs and getting zero.  And it
 * has to be in `san_grow` too, not only in `san_on_alloc`: `san_grow` is what
 * reserves the room the canary is written into, so if it sits out the first
 * allocation while `san_on_alloc` does not, the canary goes past the end of a
 * block that has no room for it.
 *
 * \~spanish EL MONTAJE VA PRIMERO, EN LAS DOS ENTRADAS, y no es un detalle: el
 * nivel arranca en `Off` y solo pasa a ser lo que pidio el entorno dentro de
 * `ensure_ready`.  Mirar el nivel antes de ejecutarlo significa que la
 * respuesta es siempre `Off`, el comprobador no arranca nunca, y da una corrida
 * limpia sobre un programa lleno de fallos -- que es el unico modo de fallo que
 * un comprobador no puede tener --.  Se descubrio apuntandolo a cuatro fallos a
 * proposito y sacando cero.  Y tiene que estar tambien en `san_grow`, no solo
 * en `san_on_alloc`: `san_grow` es quien reserva el sitio donde se escribe el
 * canario, asi que si se salta la primera reserva y `san_on_alloc` no, el
 * canario acaba pasado el final de un bloque que no tiene sitio para el.  \~ */
/**
 * @brief
 * \~english The one entry the allocator calls, which decides everything else.
 * \~spanish La unica entrada que llama el asignador, y que decide todo lo
 *           demas.
 * \~
 *
 * \~english
 * ONE OUT-OF-LINE CALL, and the reason is measured: with three -- one to grow
 * the request, one to try the guarded path, one to record the block -- an
 * allocation cost 12.4 ns against the 4.9 of a build without the checker, with
 * the checker switched OFF at run time.  Two thirds of that was asking three
 * separate times whether there was anything to do.
 *
 * The set-up question is asked ONCE here, and when the answer is no the whole
 * thing is a call, a compare and the ordinary path.
 *
 * \~spanish
 * UNA LLAMADA FUERA DE LINEA, y la razon esta medida: con tres -- una para
 * crecer la peticion, otra para probar el camino con guarda, otra para apuntar
 * el bloque -- una reserva costaba 12,4 ns frente a los 4,9 de un build sin
 * comprobador, y con el comprobador APAGADO en ejecucion.  Dos tercios de eso
 * era preguntar tres veces por separado si habia algo que hacer.
 *
 * La pregunta del montaje se hace UNA vez aqui, y cuando la respuesta es que no
 * todo esto son una llamada, una comparacion y el camino de siempre.
 * \~
 */
/* \~english The three that used to be public and are now only reachable
 * through the door above.  Declared here because that door is defined first,
 * and it reads better first: it is the one thing the allocator knows about.
 *
 * \~spanish Las tres que eran publicas y ahora solo se alcanzan por la puerta
 * de arriba.  Declaradas aqui porque esa puerta se define antes, y antes se lee
 * mejor: es lo unico que el asignador conoce.  \~ */
size_t san_grow(size_t n) noexcept;
void *san_alloc_guarded(size_t n, size_t align, const void *pc,
                        const void *fp) noexcept;
void san_on_alloc(void *p, size_t req, const void *pc, const void *fp) noexcept;

[[gnu::noinline]] void *san_alloc(size_t n) noexcept {
    /* \~english READ ONCE, HERE, WHERE IT IS STILL TRUE.  `host_alloc` is
     * inlined into its caller, so the return address of THIS call is the point
     * in the program where the allocation happened -- the site the report has
     * to name.  One step further in it would be an address inside the checker,
     * which is why it travels as an argument from here on.  `noinline` is
     * load-bearing for the same reason: inlined, there would be no return
     * address of its own to read.  And the frame goes with it: from here the
     * chain leads to the program, from one step deeper it leads through the
     * checker.
     *
     * \~spanish LEIDOS UNA VEZ, AQUI, DONDE TODAVIA SON CIERTOS.  `host_alloc`
     * va en linea dentro de quien llama, asi que la direccion de retorno de
     * ESTA llamada es el punto del programa donde ocurrio la reserva -- el
     * sitio que el informe tiene que nombrar --.  Un paso mas adentro seria una
     * direccion del comprobador, y por eso viaja como argumento a partir de
     * aqui.  El `noinline` sostiene lo mismo: en linea no habria direccion de
     * retorno propia que leer.  Y el marco va con ella: desde aqui la cadena
     * lleva al programa, un paso mas adentro lleva por el comprobador.  \~ */
    const void *const pc = __builtin_return_address(0);
    const void *const fp = __builtin_frame_address(0);

    /* \~english THE ONE QUESTION, asked once.  Switched off, this whole mode is
     * a call, a compare and the ordinary path -- which is what it cost
     * measuring: three separate hooks each asking the same thing added 7.5 ns
     * to every allocation for nothing.
     *
     * \~spanish LA UNICA PREGUNTA, hecha una vez.  Apagado, todo este modo son
     * una llamada, una comparacion y el camino de siempre -- que es lo que
     * costo medirlo: tres ganchos preguntando cada uno lo mismo anadian 7,5 ns
     * a cada reserva para nada.  \~ */
    if (!ensure_config() || detail::g_san_level == SanLevel::Off)
        return detail::alloc_body(n);

    void *p = san_alloc_guarded(n, kAlign, pc, fp);
    if (p == nullptr) p = detail::alloc_body(san_grow(n));
    san_on_alloc(p, n, pc, fp);
    return p;
}

/**
 * @brief
 * \~english A guarded block placed at an alignment the caller chose.
 * \~spanish Un bloque con guarda colocado en la alineacion que pidio quien
 *           llama.
 * \~
 *
 * \~english
 * WHY THIS EXISTS AND AN INTERIOR-POINTER LOOKUP DOES NOT.  The allocator's
 * aligned entry serves an over-sized block and RAISES the pointer inside it,
 * because a class block cannot start wherever the caller wants.  That is right
 * for the allocator and wrong for this level: an entry here is keyed by the
 * address handed out, so a raised pointer is one this level has never heard of
 * -- `ours` says no, and `host_free` panics over a block it served itself.
 *
 * This level has no such constraint.  It owns whole pages per block and picks
 * where inside them the block sits, so it can honour the alignment when placing
 * it and the address handed out stays the key.  Nothing downstream changes:
 * release, usable size and ownership all still ask about the one pointer the
 * caller holds.
 *
 * The alternative was an index that answers for any address INSIDE a block.
 * It would have to be consulted on every release, including the overwhelming
 * majority that are not guarded at all, and it buys nothing this does not --
 * so the alignment travels down instead.
 *
 * \~spanish
 * POR QUE EXISTE ESTO Y NO UNA BUSQUEDA POR PUNTERO DE DENTRO.  La entrada
 * alineada del asignador sirve un bloque de mas y SUBE el puntero dentro de el,
 * porque un bloque de clase no puede empezar donde quiera quien llama.  Eso es
 * correcto para el asignador e incorrecto para este nivel: aqui una ficha se
 * indexa por la direccion entregada, asi que un puntero subido es uno del que
 * este nivel no ha oido hablar -- `ours` dice que no, y `host_free` entra en
 * panico por un bloque que sirvio el mismo.
 *
 * Este nivel no tiene esa atadura.  Es dueno de paginas enteras por bloque y
 * elige en que punto de ellas se pone el bloque, asi que puede respetar la
 * alineacion al colocarlo y la direccion entregada sigue siendo la clave.  Nada
 * de lo que viene despues cambia: soltar, tamano utilizable y pertenencia
 * siguen preguntando por el unico puntero que tiene quien llama.
 *
 * La alternativa era un indice que contestara por cualquier direccion DE DENTRO
 * de un bloque.  Habria que consultarlo en cada liberacion, incluida la inmensa
 * mayoria que no lleva guarda, y no compra nada que esto no de -- asi que lo
 * que baja es la alineacion.  \~
 *
 * @param n     \~english useful bytes.  \~spanish bytes utiles.  \~
 * @param align \~english a power of two.  \~spanish potencia de dos.  \~
 * @return \~english the block, or nullptr when this level is not serving it,
 *         which means the caller does what it always did.
 *         \~spanish el bloque, o nullptr cuando este nivel no lo sirve, que
 *         significa que quien llama haga lo de siempre.  \~
 */
[[gnu::noinline]] void *san_alloc_aligned(size_t n, size_t align) noexcept {
    const void *const pc = __builtin_return_address(0);
    const void *const fp = __builtin_frame_address(0);
    if (!ensure_config() || detail::g_san_level == SanLevel::Off)
        return nullptr;
    void *p = san_alloc_guarded(n, align, pc, fp);
    if (p != nullptr) san_on_alloc(p, n, pc, fp);
    return p;
}

size_t san_grow(size_t n) noexcept {
    if (!ensure_ready()) return n;
    if (detail::g_san_level < SanLevel::Canary) return n;
    if (n > size_t(-1) - kCanaryBytes) return n; // no wrapping, ever
    return n + kCanaryBytes;
}

/**
 * @brief
 * \~english A block on pages of its own, flush against a page that is not
 *           mapped.
 * \~spanish Un bloque en paginas propias, pegado a una pagina sin mapear.
 * \~
 *
 * \~english
 * HOW IT CATCHES THE WRITE AND NOT ITS CONSEQUENCE.  The pages are asked for
 * with one MORE at the end, and that last one is left unmapped.  The block is
 * then put at the far end of the mapped ones, so `p + req` lands on the guard:
 * a write one byte past the end touches memory that does not exist and the
 * process faults THERE, with the real address and the real stack, instead of
 * quietly corrupting a neighbour and being found out somewhere else an hour
 * later.  No compiler pass anywhere -- this is the hardware doing the check.
 *
 * THE GAP, and it is why the canary is still written.  What comes back has to
 * be aligned like any other allocation, so the block is pushed DOWN to the
 * alignment and that leaves up to fifteen bytes between its end and the guard.
 * An overflow that small lands in the gap and the page never notices.  Those
 * bytes carry the canary, so the gap is checked when the block is released --
 * small overflows late, big ones instantly, and nothing in between missed.
 *
 * WHAT IT COSTS, and it is not a detail: the smallest allocation there is takes
 * two pages, and the range is NEVER given back -- releasing only takes the
 * pages away, so the addresses stay spent and a use-after-free keeps faulting
 * for the life of the process.  That is the point, and it is also why this is
 * nobody's default.
 *
 * \~spanish
 * COMO CAZA LA ESCRITURA Y NO SU CONSECUENCIA.  Se piden las paginas con UNA
 * mas al final, y esa ultima se deja sin mapear.  El bloque se pone al fondo de
 * las mapeadas, asi que `p + req` cae en la guarda: una escritura un byte mas
 * alla toca memoria que no existe y el proceso falla AHI, con la direccion de
 * verdad y la pila de verdad, en vez de corromper al vecino en silencio y
 * descubrirse en otro sitio una hora despues.  Sin ningun pase de compilador --
 * la comprobacion la hace el hardware.
 *
 * EL HUECO, y es por lo que el canario sigue puesto.  Lo que se devuelve hay
 * que alinearlo como cualquier otra reserva, asi que el bloque se empuja hacia
 * ABAJO y quedan hasta quince bytes entre su final y la guarda.  Un
 * desbordamiento tan corto cae en el hueco y la pagina no se entera.  Esos
 * bytes llevan canario, asi que el hueco se mira al soltar el bloque -- los
 * pequenos tarde, los grandes al instante, y ninguno en medio perdido.
 *
 * LO QUE CUESTA, y no es un detalle: la reserva mas pequena que hay se lleva
 * dos paginas, y el rango NO se devuelve jamas -- soltar solo quita las
 * paginas, asi que las direcciones quedan gastadas y un uso despues de liberar
 * sigue fallando el resto del proceso.  Eso es lo que se compra, y es tambien
 * por lo que esto no es el defecto de nadie.
 * \~
 */
void *san_alloc_guarded(size_t n, size_t align, const void *pc,
                        const void *fp) noexcept {
    /* \~english The CONFIG and not the whole set-up: a guarded block does not
     * touch the shadow, so waiting for the shadow would keep the very first
     * allocation of the process -- and in a small program, every allocation --
     * out of the level that was asked for.
     *
     * \~spanish Los AJUSTES y no el montaje entero: un bloque con guarda no
     * toca el sombreado, asi que esperar al sombreado dejaria la primerisima
     * reserva del proceso -- y en un programa pequeno, todas -- fuera del nivel
     * que se habia pedido.  \~ */
    if (!ensure_config() || detail::g_san_level < SanLevel::Guard)
        return nullptr;
    if (g_guarded == nullptr || n == 0) return nullptr;

    /* \~english BEFORE ASKING THE SYSTEM FOR ANYTHING.  Past this point the
     * block gets its pages reserved and committed, and only then is there an
     * address to key an entry by -- so a refusal from the table arrives with
     * three trips into the kernel already paid and nothing to show for them.
     * The entries are never taken back, so once the table has stopped placing
     * anything it will not start again, and going on asking buys nothing.  The
     * count below is the same one the report prints, so what the run could not
     * cover is still said in full: what is dropped here is the waste, not the
     * telling.  See @c kGuardGiveUp.
     *
     * \~spanish ANTES DE PEDIRLE NADA AL SISTEMA.  A partir de aqui el bloque
     * consigue sus paginas reservadas y comprometidas, y solo entonces hay una
     * direccion con la que dar de alta la ficha -- asi que una negativa de la
     * tabla llega con tres viajes al nucleo ya pagados y nada que ensenar.  Las
     * fichas no se recuperan nunca, asi que cuando la tabla ha dejado de
     * colocar no va a volver a empezar, y seguir preguntando no compra nada.
     * La cuenta de abajo es la misma que imprime el informe, asi que lo que la
     * corrida no pudo cubrir se sigue diciendo entero: lo que se deja de hacer
     * aqui es el desperdicio, no el aviso.  Ver @c kGuardGiveUp.  \~ */
    uint32_t skip = g_guard_skip.load(std::memory_order_relaxed);
    while (skip != 0 && !g_guard_skip.compare_exchange_weak(
                            skip, skip - 1, std::memory_order_relaxed,
                            std::memory_order_relaxed)) {
        /* Somebody else took this one; `skip` now holds what is left. */
    }
    if (skip != 0) {
        g_guard_full.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    /* \~english THE THREAD STILL NEEDS ITS CACHE, even though this block will
     * not come out of it.  The allocator creates that cache lazily, on the
     * first call that goes through `alloc_body` -- and a guarded block never
     * goes through it.  So a thread whose allocations are all served here never
     * gets one, and everything that hangs off it goes quiet: `record_alloc_site`
     * gives up, the tags stop counting, and the per-thread counters stay at
     * zero.  The allocator's own report goes blind exactly when the strictest
     * checking mode is on, and it goes blind WITHOUT SAYING SO -- the report
     * still prints, with nothing in it.
     *
     * Measured rather than reasoned: `alloc_sites_skipped()` reads 0 at the
     * poison level and 1 at this one, for the same program and the same
     * allocation.  It is what makes five of this library's own tests fail here
     * -- sites, tags, counters, call sites and the CSV export -- with one cause.
     *
     * `ensure_cache` is the allocator's own entry point for this and it is
     * exported for exactly this reach, so nothing there changes.  It costs a
     * slot read and a branch, on a path that has just asked the system to
     * reserve and commit pages.
     *
     * \~spanish EL HILO SIGUE NECESITANDO SU CACHE, aunque este bloque no vaya
     * a salir de ella.  El asignador la crea perezosamente, en la primera
     * llamada que pasa por `alloc_body` -- y un bloque con guarda no pasa por
     * ahi nunca.  Asi que un hilo cuyas reservas se sirvan todas aqui no llega
     * a tener ninguna, y todo lo que cuelga de ella se calla:
     * `record_alloc_site` se rinde, las etiquetas dejan de contar y los
     * contadores por hilo se quedan a cero.  El informe del propio asignador se
     * queda ciego justo cuando el modo mas estricto esta puesto, y se queda
     * ciego SIN DECIRLO -- el informe sigue saliendo, vacio.
     *
     * Medido y no razonado: `alloc_sites_skipped()` da 0 en el nivel de veneno
     * y 1 en este, con el mismo programa y la misma reserva.  Es lo que hace
     * fallar aqui a cinco de los tests de esta libreria -- sitios, etiquetas,
     * contadores, sitios de llamada y el volcado CSV -- con una sola causa.
     *
     * `ensure_cache` es la entrada del propio asignador para esto y esta
     * exportada justo para este alcance, asi que alli no cambia nada.  Cuesta
     * leer una ranura y una rama, en un camino que acaba de pedirle al sistema
     * que reserve y comprometa paginas.  \~ */
    const detail::ThreadCache *const tc = detail::ensure_cache();

    if (align < kAlign) align = kAlign;
    if ((align & (align - 1)) != 0) return nullptr; // not a power of two

    const size_t page = os_page_size();
    /* \~english The room the alignment needs.  The block is placed by moving it
     * INSIDE these pages, so an alignment finer than a page always fits in what
     * the rounding up already leaves over; a coarser one has to be asked for,
     * or the move would push the block past the pages that were committed.
     *
     * \~spanish El sitio que pide la alineacion.  El bloque se coloca moviendolo
     * DENTRO de estas paginas, asi que una alineacion mas fina que una pagina
     * cabe siempre en lo que la redondeo al alza ya deja de sobra; una mas
     * gruesa hay que pedirla, o el movimiento empujaria el bloque mas alla de
     * las paginas comprometidas.  \~ */
    const size_t slack = align > page ? align : 0;
    if (n > size_t(-1) - kCanaryBytes - slack - page) return nullptr;
    const size_t data = (n + kCanaryBytes + slack + page - 1) / page * page;
    const size_t total = data + page; // the guard
    if (data < n) return nullptr;     // wrapped: refuse rather than serve wrong

    /* \~english Reserved WITHOUT permissions and then only the data pages
     * committed: what is left is the guard, and it is unmapped because nobody
     * ever asked for it, not because something took it away.
     *
     * \~spanish Reservado SIN permisos y luego comprometidas solo las paginas
     * de datos: lo que queda es la guarda, y esta sin mapear porque nadie la
     * pidio nunca, no porque algo se la quitara.  \~ */
    void *base = os_reserve(total);
    if (base == nullptr) return nullptr;

    unsigned char *const start = static_cast<unsigned char *>(base);
    const bool under = g_guard_edge == SanGuard::Underflow;
    /* \~english WHICH PAGE IS LEFT OUT IS WHICH EDGE IS WATCHED.  Watching the
     * end means the spare page goes last, so committing from the base is right.
     * Watching the START means it goes FIRST, and committing from the base
     * would commit the very page that has to stay missing -- the read before
     * the block would go through, and the block's own last page would be the
     * one left unmapped instead.  The guard would be at the wrong end and say
     * nothing.
     *
     * \~spanish QUE PAGINA SE DEJA FUERA ES QUE BORDE SE VIGILA.  Vigilar el
     * final quiere decir que la pagina de sobra va al final, asi que comprometer
     * desde la base es correcto.  Vigilar el PRINCIPIO quiere decir que va
     * DELANTE, y comprometer desde la base comprometeria justo la pagina que
     * tiene que faltar -- la lectura de antes del bloque pasaria, y la que se
     * quedaria sin mapear seria la ultima del propio bloque.  La guarda estaria
     * en el extremo equivocado y no diria nada.  \~ */
    unsigned char *const mapped = under ? start + page : start;
    if (!os_commit(mapped, data, kOsReadWrite)) {
        os_free(base, total);
        return nullptr;
    }

    unsigned char *p;
    size_t tail;
    if (under) {
        /* \~english The other edge: the block starts where the mapped pages
         * start, so reading or writing BEFORE it is what faults.  Then the
         * guard is the page before, and nothing watches the end.
         *
         * \~spanish El otro borde: el bloque empieza donde empiezan las paginas
         * mapeadas, asi que lo que falla es leer o escribir ANTES de el.
         * Entonces la guarda es la pagina de delante, y nadie vigila el final.
         * \~ */
        const uintptr_t first = reinterpret_cast<uintptr_t>(mapped);
        p = reinterpret_cast<unsigned char *>((first + align - 1) &
                                              ~uintptr_t(align - 1));
        tail = 0;
    } else {
        const uintptr_t end = reinterpret_cast<uintptr_t>(mapped) + data;
        const uintptr_t want = (end - n) & ~uintptr_t(align - 1);
        p = reinterpret_cast<unsigned char *>(want);
        tail = size_t(end - want - n);
    }

    Guarded *g = guard_insert(p);
    if (g == nullptr) { // the table is full: say nothing false, serve nothing
        os_free(base, total);
        return nullptr;
    }

    const void *frames[kFrames];
    bool walked = false;
    const unsigned nf = walk_stack(frames, pc, fp, &walked);
    g->base = base;
    g->total = total;
    g->req = n;
    g->tail = tail;
    /* Stamped where the block is BORN, which is the only place that knows.  See
     * the two fields in `Guarded`. */
    g->seq = thread_allocs(tc);
    g->thread = detail::have_cache(tc) ? tc->id : 0;
    /* AFTER the stamp, so the stamp is the count BEFORE this block and the next
     * one reads one more.  See `g_guard_thread_allocs`. */
    if (detail::have_cache(tc) && tc->id < util::kMaxThreads)
        g_guard_thread_allocs[tc->id].fetch_add(1, std::memory_order_relaxed);
    /* \~english BEFORE THE BLOCK LEAVES, so that an address inside it can be
     * placed from the first instant it exists.  Written after the fields it
     * points at, because the lookup reads them.
     *
     * \~spanish ANTES DE QUE EL BLOQUE SALGA, para que una direccion de dentro
     * se pueda situar desde el primer instante en que existe.  Escrito despues
     * de los campos a los que apunta, porque la busqueda los lee.  \~ */
    index_insert(base, total, uint32_t(g - g_guarded));
    g->alloc_stack = intern_stack(frames, nf, walked);
    /* \~english AND HERE TOO, because at the guard level this is the ONLY place
     * that sees the block.  A guarded block lives on pages of its own, outside
     * the region, so `slot_of` refuses it and the shadow never hears about it
     * -- which means `san_on_alloc` does not count it either.  Leaving this out
     * would make the volume figures shrink as the level goes UP: the strictest
     * mode reporting the least, and reporting it without a word.  That is the
     * failure this whole mode exists to not have.
     *
     * \~spanish Y AQUI TAMBIEN, porque en el nivel de guarda este es el UNICO
     * sitio que ve el bloque.  Un bloque guardado vive en paginas propias,
     * fuera de la region, asi que `slot_of` lo rechaza y el sombreado no se
     * entera -- con lo que `san_on_alloc` tampoco lo cuenta.  Dejarlo fuera
     * haria que las cifras de volumen ENCOGIERAN al SUBIR el nivel: el modo mas
     * estricto informando de menos, y sin decirlo.  Ese es justo el fallo que
     * este modo existe para no tener.  \~ */
    note_birth(g->alloc_stack, n, true);
    g->free_stack = 0;
    g->state = kStAlive;
    g_guard_bytes.fetch_add(total, std::memory_order_relaxed);
    /* And the BLOCK, which is what closes the identity with the allocator's own
     * count.  See `g_guard_blocks`. */
    g_guard_blocks.fetch_add(1, std::memory_order_relaxed);
    /* \~english And under WHICH purpose, read from the cache this function made
     * sure exists.  Without a cache the tag would be "not known" for every
     * guarded block, and the split would look like it worked while saying
     * nothing.  See `g_guard_by_tag`.
     *
     * \~spanish Y bajo QUE proposito, leido de la cache que esta misma funcion
     * se ha asegurado de que exista.  Sin cache la etiqueta seria "no se" en
     * todos los bloques con guarda, y el reparto pareceria funcionar sin decir
     * nada.  Ver `g_guard_by_tag`.  \~ */
    {
        const unsigned tag = detail::have_cache(tc) ? unsigned(tc->tag) : 0u;
        if (tag < VESTA_ALLOC_TAG_SLOTS)
            g_guard_by_tag[tag].fetch_add(1, std::memory_order_relaxed);
    }

    /* The gap between the end of the block and the guard, filled so that an
     * overflow too small to reach the page still leaves a mark. */
    if (tail != 0) std::memset(p + n, kCanaryByte, tail);
    return p;
}

/// @return true when the release was handled here and must not go any further.
bool guarded_free(void *p, const void *fp) noexcept {
    Guarded *g = guard_find(p);
    if (g == nullptr) {
        /* \~english OR AN ADDRESS INSIDE ONE.  `host_free` takes any address
         * within a block the system served -- that is what `direct_take` does
         * for the allocator -- so a release from the middle is correct code,
         * and this level has to recognise it or stop the process over it.  The
         * canary and the double-release verdict below then read the BLOCK's
         * fields, which is what they were always about.
         *
         * \~spanish O UNA DIRECCION DE DENTRO.  `host_free` acepta cualquier
         * direccion dentro de un bloque que sirvio el sistema -- eso es lo que
         * hace `direct_take` para el asignador --, asi que soltar desde el medio
         * es codigo correcto, y este nivel tiene que reconocerlo o parar el
         * proceso por ello.  El canario y el veredicto de doble liberacion de
         * abajo leen entonces los campos del BLOQUE, que es de lo que siempre
         * iban.  \~ */
        g = guard_locate(p);
        if (g == nullptr) return false;
        p = const_cast<void *>(g->p.load(std::memory_order_acquire));
    }

    if (g->state == kStFreed) {
        verdict(Certainty::Proven, "released twice", p);
        print_stack("allocated", g->alloc_stack);
        print_stack("released the first time", g->free_stack);
        return true; // and NOT again: the pages are already gone
    }

    for (size_t i = 0; i < g->tail; ++i) {
        if (static_cast<unsigned char *>(p)[g->req + i] != kCanaryByte) {
            verdict(Certainty::Proven, "written past the end of the block", p);
            std::fprintf(stderr,
                         "    asked for %zu bytes; it overran into the %zu that "
                         "sit between the block and the guard page, from byte "
                         "%zu.  A longer overrun would have faulted where it "
                         "happened.\n",
                         g->req, g->tail, i);
            print_stack("allocated", g->alloc_stack);
            break;
        }
    }

    const void *frames[kFrames];
    bool walked = false;
    const unsigned nf =
        walk_stack(frames, __builtin_return_address(0),
                   __builtin_frame_address(0), &walked);
    g->free_stack = intern_stack(frames, nf, walked);
    g->state = kStFreed;
    /* Counted where it is DONE, so that the two ledgers add up to the whole.
     * See `g_guard_frees`. */
    g_guard_frees.fetch_add(1, std::memory_order_relaxed);

    /* \~english WHAT THIS BLOCK TURNED OUT TO BE, the same question the shadow
     * answers for the blocks it holds -- and here, where it dies, is the only
     * place that knows.  Counted the same way and against the same counter, so
     * a life measured at this level means what a life measured at any other one
     * means.  Released on another thread it is counted as unknown rather than
     * folded in, which `note_death` does from `same_thread`: subtracting one
     * thread's counter from another's would give a number with the shape of a
     * life that is not one.
     *
     * \~spanish LO QUE ESTE BLOQUE RESULTO SER, la misma pregunta que el
     * sombreado contesta por los bloques que guarda el -- y aqui, donde muere,
     * es el unico sitio que lo sabe.  Contado igual y contra el mismo contador,
     * asi que una vida medida en este nivel significa lo mismo que una medida
     * en cualquier otro.  Soltado en otro hilo se cuenta como desconocida en vez
     * de mezclarse, cosa que hace `note_death` a partir de `same_thread`:
     * restar el contador de un hilo al de otro daria un numero con forma de
     * vida que no lo es.  \~ */
    const detail::ThreadCache *const tc = detail::current_cache();
    const uint32_t tid = detail::have_cache(tc) ? tc->id : 0;
    note_death(g->alloc_stack, uint32_t(g->req), g->thread == tid, g->seq,
               thread_allocs(tc));
    /* Y EL PAR, tambien desde esta puerta.  Un bloque con guarda no pasa por el
     * sombreado, asi que engancharlo solo alli dejaria la tabla muda justo en el
     * nivel mas estricto -- y muda sin decirlo.  Ver `note_pair`. */
    note_pair(g->alloc_stack, g->free_stack, uint32_t(g->req));

    /* \~english DECOMMITTED, NOT FREED.  The pages go, so touching the block
     * from now on faults where it is touched; the range stays ours, so the
     * address is never handed to anybody else and the fault is always about
     * THIS block.  That is what makes a use-after-free point at the right code
     * -- and what makes this level expensive.
     *
     * \~spanish DESCOMPROMETIDO, NO LIBERADO.  Las paginas se van, asi que
     * tocar el bloque a partir de ahora falla donde se toca; el rango sigue
     * siendo nuestro, asi que la direccion no se le entrega a nadie mas y el
     * fallo siempre es sobre ESTE bloque.  Eso es lo que hace que un uso
     * despues de liberar apunte al codigo correcto -- y lo que hace caro este
     * nivel.  \~ */
    /* \~english The pages to drop are the MAPPED ones, and which end they start
     * at is the edge -- the same asymmetry that decides where the spare page
     * goes when the block is served.  Handing the base in unconditionally would
     * ask the system to drop the guard page and keep the block's last one.
     *
     * \~spanish Las paginas que se sueltan son las MAPEADAS, y en que extremo
     * empiezan lo dice el borde -- la misma asimetria que decide donde va la
     * pagina de sobra al servir el bloque.  Entregar la base sin mirar seria
     * pedirle al sistema que soltara la de guarda y se quedara con la ultima
     * del bloque.  \~ */
    const size_t page = os_page_size();
    unsigned char *const mapped =
        static_cast<unsigned char *>(const_cast<void *>(g->base)) +
        (g_guard_edge == SanGuard::Underflow ? page : 0);
    os_decommit(mapped, g->total - page);
    return true;
}

void san_on_alloc(void *p, size_t req, const void *pc,
                  const void *fp) noexcept {
    if (p == nullptr) return;

    /* \~english A guarded block wrote its own entry when it was served, and it
     * does not live in the region, so the shadow would count it as something it
     * could not look at -- which would be a lie in the other direction.
     *
     * \~spanish Un bloque con guarda escribio su propia entrada al servirse, y
     * no vive en la region, asi que el sombreado lo contaria como algo que no
     * pudo mirar -- que seria mentir en el otro sentido.  \~ */
    if (detail::g_san_level >= SanLevel::Guard && guard_find(p) != nullptr)
        return;

    if (!ensure_ready()) return;
    size_t block = 0;
    Slot *s = slot_of(p, &block);
    if (s == nullptr) {
        /* \~english OUTSIDE THE SHADOW, AND STILL COUNTED -- which is the whole
         * of what changed here.  A block over the small-class limit lives on a
         * reservation of its own, so there is no slot to put a canary in, no
         * poison to compare and no life to measure.  None of that is needed to
         * say WHERE IT CAME FROM and HOW BIG IT WAS, and until now this
         * function gave up one line too early and lost both.
         *
         * WHY IT MATTERS MORE THAN THE COUNT SUGGESTS: measured on a real
         * compile, the blocks out here were FORTY-ONE operations and the three
         * largest allocations in the program -- 408 MiB in twenty reallocs of
         * one vector, 173 MiB in one, 114 MiB in one.  A volume list that stops
         * at the boundary ranks the small-class traffic and calls it "what
         * moved the most", with the actual heavyweights on the other side.
         * That is not a gap in coverage, it is a report that answers a
         * different question than its heading claims.
         *
         * AND IT IS NOT A COST.  These are rare by construction -- twenty calls
         * for four hundred megabytes -- so a stack walk each is nothing, and
         * the path it sits on was already the one that did no work.
         *
         * \~spanish FUERA DEL SOMBREADO, Y AUN ASI CONTADO -- que es todo lo
         * que cambio aqui.  Un bloque que pasa del limite de clase pequena vive
         * en una reserva propia, asi que no hay ranura donde poner un canario,
         * ni veneno que comparar, ni vida que medir.  Nada de eso hace falta
         * para decir DE DONDE VINO y CUANTO MEDIA, y hasta ahora esta funcion
         * se rendia una linea antes y perdia las dos cosas.
         *
         * POR QUE IMPORTA MAS DE LO QUE LA CUENTA SUGIERE: medido en una
         * compilacion de verdad, los bloques de aqui fuera eran CUARENTA Y UNA
         * operaciones y las tres mayores reservas del programa -- 408 MiB en
         * veinte realojos de un vector, 173 MiB en uno, 114 MiB en uno.  Una
         * lista de volumen que se para en la frontera ordena el trafico de
         * clase pequena y lo llama "lo que mas movio", con los pesos pesados de
         * verdad al otro lado.  Eso no es un hueco de cobertura, es un informe
         * que contesta otra pregunta distinta de la que dice su titulo.
         *
         * Y NO CUESTA.  Son raros por construccion -- veinte llamadas para
         * cuatrocientos megabytes --, asi que un recorrido de pila cada uno no
         * es nada, y el camino en el que va era justo el que no hacia nada.
         * \~ */
        g_uncovered.fetch_add(1, std::memory_order_relaxed);
        const void *big[kFrames];
        bool big_walked = false;
        const unsigned bn = walk_stack(big, pc, fp, &big_walked);
        note_birth(intern_stack(big, bn, big_walked), req, false);
        return;
    }

    /* \~english A WRITE AFTER FREE, caught where it can be caught without a
     * page each: the block was poisoned when it was released, so anything that
     * is not the poison now was written by somebody who no longer owned it.
     *
     * \~spanish UNA ESCRITURA DESPUES DE LIBERAR, cazada donde se puede cazar
     * sin una pagina por bloque: el bloque se enveneno al soltarlo, asi que
     * todo lo que ahora no sea el veneno lo escribio alguien que ya no era su
     * dueno.  \~ */
    if (detail::g_san_level >= SanLevel::Poison &&
        meta_state(s->meta) == kStFreed && g_poison != SanPoison::None) {
        const size_t from = poison_from();
        const size_t len = poison_len(block);
        const unsigned char *b = static_cast<const unsigned char *>(p);
        for (size_t i = 0; i < len; ++i) {
            if (b[from + i] != kPoisonByte) {
                verdict(Certainty::Proven,
                        "written into a block after it was released", p);
                std::fprintf(stderr, "    at byte %zu of the %u it held\n",
                             from + i, s->req);
                print_stack("allocated", s->alloc_stack);
                print_stack("released", s->free_stack);
                break;
            }
        }
    }

    const void *frames[kFrames];
    bool walked = false;
    const unsigned n = walk_stack(frames, pc, fp, &walked);

    const detail::ThreadCache *c = detail::current_cache();
    const uint32_t tid = detail::have_cache(c) ? c->id : 0;

    s->alloc_stack = intern_stack(frames, n, walked);
    /* \~english AND WHAT IT MOVED, added up here because here is the only place
     * that sees it.  The shadow forgets a block as soon as its address is used
     * again, so by the end of the run it can say what leaked and cannot say
     * what a site allocated.  See @c note_birth.
     *
     * \~spanish Y CUANTO MOVIO, sumado aqui porque aqui es el unico sitio que
     * lo ve.  El sombreado olvida un bloque en cuanto su direccion se vuelve a
     * usar, asi que al acabar la corrida sabe decir que se fugo y no sabe decir
     * cuanto reservo un sitio.  Ver @c note_birth.  \~ */
    note_birth(s->alloc_stack, req, true);
    s->free_stack = 0;
    s->req = uint32_t(req);
    s->meta = meta_of(kStAlive, tid, 0);
    /* Its birthday, in allocations of this thread.  Read from counters the
     * allocator already keeps, so nothing new is stored anywhere. */
    s->seq = thread_allocs(c);

    /* \~english ONLY IF IT FITS, and the condition is checked and not assumed.
     * On the very first allocation of the process `san_grow` sits out -- the
     * region it needs does not exist yet -- while this call, one instant later,
     * finds it ready.  Writing the canary then would put it past the end of a
     * block that was never grown to hold it: the checker corrupting memory.
     * The same condition is asked again when the block is released, so the two
     * sides agree without having to remember anything.
     *
     * \~spanish SOLO SI CABE, y la condicion se comprueba, no se da por hecha.
     * En la primerisima reserva del proceso `san_grow` se queda fuera -- la
     * region que necesita todavia no existe -- mientras que esta llamada, un
     * instante despues, ya la encuentra montada.  Escribir el canario entonces
     * lo pondria pasado el final de un bloque que nunca crecio para tenerlo: el
     * comprobador corrompiendo memoria.  La misma condicion se pregunta otra
     * vez al soltar el bloque, asi que los dos lados coinciden sin tener que
     * recordar nada.  \~ */
    if (detail::g_san_level >= SanLevel::Canary && canary_fits(req, block))
        write_canary(p, req);
}

[[gnu::noinline]] bool san_on_free(void *p) noexcept {
    if (p == nullptr) return true;
    if (!ensure_config() || detail::g_san_level == SanLevel::Off) return true;

    /* \~english A guarded block never goes back to the allocator: it was never
     * served by it, and handing it over would send a pointer from outside the
     * region to `no_foreign_free`, which stops the process.  Answering false is
     * what keeps it here -- and it is asked BEFORE the shadow, which that block
     * does not have.
     *
     * \~spanish Un bloque con guarda no vuelve nunca al asignador: no lo sirvio
     * el, y entregarselo mandaria un puntero de fuera de la region a
     * `no_foreign_free`, que para el proceso.  Contestar false es lo que lo
     * mantiene aqui -- y se pregunta ANTES que el sombreado, que ese bloque no
     * tiene.  \~ */
    if (detail::g_san_level >= SanLevel::Guard &&
        guarded_free(p, __builtin_frame_address(0)))
        return false;

    if (!ensure_ready()) return true;
    size_t block = 0;
    Slot *s = slot_of(p, &block);
    if (s == nullptr) {
        g_uncovered.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    const uint32_t st = meta_state(s->meta);

    if (st == kStFreed) {
        /* \~english A DOUBLE FREE, and it is PROVEN: this exact block is
         * already on a free list.  Letting it through would push it a second
         * time and hand one block to two owners, so the checker would have
         * turned a reported bug into a corrupted heap.  That is why this
         * answers false.
         *
         * \~spanish UNA DOBLE LIBERACION, y es DEMOSTRADA: este mismo bloque ya
         * esta en una lista de libres.  Dejarla pasar lo meteria una segunda
         * vez y entregaria un bloque a dos duenos, con lo que el comprobador
         * habria convertido un fallo avisado en un monton corrompido.  Por eso
         * esto contesta false.  \~ */
        verdict(Certainty::Proven, "released twice", p);
        print_stack("allocated", s->alloc_stack);
        print_stack("released the first time", s->free_stack);
        const void *frames[kFrames];
        bool walked = false;
        const unsigned n =
            walk_stack(frames, __builtin_return_address(0),
                   __builtin_frame_address(0), &walked);
        print_stack("released again", intern_stack(frames, n, walked));
        return false;
    }

    if (st == kStNever) {
        /* \~english Ours by address, but we never saw it handed out.  That is
         * what a block allocated before the checker was ready looks like, and
         * also what a block from a door that is not instrumented looks like, so
         * it is SUSPECTED and the release goes ahead: refusing would break a
         * program that is doing nothing wrong.
         *
         * \~spanish Nuestro por la direccion, pero no lo vimos entregarse.  Eso
         * es lo que parece un bloque reservado antes de que el comprobador
         * estuviera listo, y tambien lo que parece uno que entro por una puerta
         * sin instrumentar, asi que es SOSPECHA y la liberacion sigue adelante:
         * negarse romperia un programa que no esta haciendo nada mal.  \~ */
        verdict(Certainty::Suspected,
                "released a block the checker never saw handed out", p);
        g_uncovered.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (detail::g_san_level >= SanLevel::Canary &&
        canary_fits(s->req, block)) {
        const unsigned intact = check_canary(p, s->req);
        if (intact < kCanaryBytes) {
            verdict(Certainty::Proven, "written past the end of the block", p);
            std::fprintf(stderr,
                         "    asked for %u bytes; the guard behind them was "
                         "overwritten from byte %u\n",
                         s->req, intact);
            print_stack("allocated", s->alloc_stack);
        }
    }

    const void *frames[kFrames];
    bool walked = false;
    const unsigned n =
        walk_stack(frames, __builtin_return_address(0),
                   __builtin_frame_address(0), &walked);
    const detail::ThreadCache *c = detail::current_cache();
    const uint32_t tid = detail::have_cache(c) ? c->id : 0;

    /* \~english Allocated on one thread and released on another.  NOT a
     * verdict: it is legal and the allocator handles it.  It is counted because
     * it is the shape of ownership crossing threads by accident, which is worth
     * seeing even when nothing is broken.
     *
     * \~spanish Reservado en un hilo y soltado en otro.  NO es un veredicto: es
     * legal y el asignador lo maneja.  Se cuenta porque tiene la forma de la
     * propiedad cruzando de hilo sin querer, que merece verse aunque no haya
     * nada roto.  \~ */
    const bool same_thread = meta_alloc_thread(s->meta) == tid;
    if (!same_thread) g_cross_thread.fetch_add(1, std::memory_order_relaxed);

    /* \~english WHAT THIS BLOCK TURNED OUT TO BE.  Here, where it dies, is the
     * only place that knows how long it lived -- and adding it up per site is
     * what turns the two axes from something a caller declares into something
     * the run MEASURES.
     *
     * \~spanish LO QUE ESTE BLOQUE RESULTO SER.  Aqui, donde muere, es el unico
     * sitio que sabe cuanto vivio -- y sumarlo por sitio es lo que convierte
     * los dos ejes de algo que declara quien llama en algo que la corrida MIDE.
     * \~ */
    note_death(s->alloc_stack, s->req, same_thread, s->seq, thread_allocs(c));

    s->free_stack = intern_stack(frames, n, walked);
    /* Y QUIEN LO DEVOLVIO, contra quien lo entrego.  Aqui, donde por primera y
     * unica vez se conocen los dos.  Ver `note_pair`. */
    note_pair(s->alloc_stack, s->free_stack, s->req);
    s->meta = meta_of(kStFreed, meta_alloc_thread(s->meta), tid);

    if (detail::g_san_level >= SanLevel::Poison && g_poison != SanPoison::None)
        std::memset(static_cast<unsigned char *>(p) + poison_from(),
                    kPoisonByte, poison_len(block));
    return true;
}

uint64_t san_verdicts() noexcept {
    return g_verdicts.load(std::memory_order_relaxed);
}

uint64_t san_longest_life() noexcept {
    return g_longest_life.load(std::memory_order_relaxed);
}

size_t san_guarded_size(const void *p) noexcept {
    if (p == nullptr || detail::g_san_level < SanLevel::Guard) return 0;
    const Guarded *g = guard_find(p);
    /* \~english A released block answers zero: its pages are gone, so promising
     * bytes there would be promising memory that faults on the first touch.
     * The double-free verdict is `san_on_free`'s job, not this one's -- this
     * only says how much can be written RIGHT NOW.
     *
     * \~spanish Un bloque soltado contesta cero: sus paginas ya no estan, asi
     * que prometer bytes ahi seria prometer memoria que falla al primer toque.
     * El veredicto de doble liberacion es cosa de `san_on_free`, no de esta --
     * esta solo dice cuanto se puede escribir AHORA.  \~ */
    if (g == nullptr) {
        /* \~english NOT THE ADDRESS HANDED OUT, so maybe one INSIDE the block:
         * the allocator answers those for anything it got from the system, and
         * counts what is left FROM there rather than the whole block.  This
         * only runs once the exact question has already said no, and only on a
         * pointer that is in neither region, so the ordinary path never pays
         * for it.
         *
         * \~spanish NO LA DIRECCION ENTREGADA, asi que quiza una DE DENTRO del
         * bloque: el asignador contesta por esas para todo lo que le dio el
         * sistema, y cuenta lo que queda DESDE ahi en vez del bloque entero.
         * Esto solo corre cuando la pregunta exacta ya ha dicho que no, y solo
         * sobre un puntero que no esta en ninguna de las dos regiones, asi que
         * el camino normal no lo paga nunca.  \~ */
        const Guarded *in = guard_locate(p);
        if (in == nullptr || in->state != kStAlive) return 0;
        const uintptr_t start =
            reinterpret_cast<uintptr_t>(in->p.load(std::memory_order_acquire));
        return in->req - (reinterpret_cast<uintptr_t>(p) - start);
    }
    if (g->state != kStAlive) return 0;
    return g->req;
}

bool san_realloc(void *p, size_t n, void **out) noexcept {
    if (p == nullptr || detail::g_san_level < SanLevel::Guard) return false;
    Guarded *g = guard_find(p);
    if (g == nullptr) return false;
    if (g->state != kStAlive) {
        /* \~english Growing a block that was already released is a use after
         * free with a different verb, and it gets the same answer: it is SAID
         * and nothing is served.  Handing back memory here would turn a caught
         * bug into a working program that is wrong.
         *
         * \~spanish Hacer crecer un bloque ya soltado es un uso despues de
         * liberar con otro verbo, y recibe la misma respuesta: se DICE y no se
         * sirve nada.  Devolver memoria aqui convertiria un fallo cazado en un
         * programa que funciona y esta mal.  \~ */
        verdict(Certainty::Proven, "resized after it was released", p);
        print_stack("allocated", g->alloc_stack);
        print_stack("released", g->free_stack);
        *out = nullptr;
        return true;
    }

    const size_t old = g->req;
    if (n <= old) {
        /* \~english It already holds it, and the block does NOT shrink: moving
         * it would move the guard page with it, and the point of this level is
         * that the page sits exactly at the end the caller asked for.  Shrinking
         * in place would leave the guard beyond the new end and stop catching
         * the overrun this level exists to catch.
         *
         * \~spanish Ya lo tiene, y el bloque NO encoge: moverlo moveria con el
         * la pagina de guarda, y la gracia de este nivel es que la pagina esta
         * justo al final que pidio quien llama.  Encoger en el sitio dejaria la
         * guarda mas alla del nuevo final y dejaria de cazar el desbordamiento
         * para el que existe este nivel.  \~ */
        *out = p;
        return true;
    }

    /* \~english A guarded block cannot grow where it lies -- its pages end at a
     * guard that must stay at the end -- so a bigger one is taken, the contents
     * are copied and the old one is released THROUGH THE CHECKER, which is what
     * keeps the use-after-free watch on the old address.  Growing it through
     * the allocator instead would hand a pointer from outside the region to
     * `no_foreign_free` and stop the process, which is exactly the hole this
     * closes.
     *
     * \~spanish Un bloque con guarda no puede crecer donde esta -- sus paginas
     * acaban en una guarda que tiene que quedarse al final --, asi que se coge
     * uno mayor, se copia el contenido y el viejo se suelta POR EL COMPROBADOR,
     * que es lo que mantiene la vigilancia de uso-tras-liberar sobre la
     * direccion vieja.  Hacerlo crecer por el asignador mandaria un puntero de
     * fuera de la region a `no_foreign_free` y pararia el proceso, que es
     * justamente el agujero que esto cierra.  \~ */
    void *q = san_alloc_guarded(n, kAlign, __builtin_return_address(0),
                                __builtin_frame_address(0));
    if (q == nullptr) {
        /* \~english NO GUARD LEFT, SO A PLAIN BLOCK -- and it has to be, which
         * cost a diagnosis to learn.  The guard table holds `kGuardSlots`
         * entries and a released block NEVER gives its slot back: its pages are
         * decommitted and the range kept, because that is what makes a
         * use-after-free point at the right code.  So on a real workload the
         * table fills -- measured here at 498.554 refusals and 6,4 GB of
         * address space held -- and from then on every guarded block that wants
         * to grow arrives at this line.
         *
         * Reporting failure here was wrong and it is what kept the crash alive
         * after the first fix: `realloc` answered null, `pthread_key_create`
         * turned that into ENOMEM, and `emutls_init` called `abort`.  The
         * allocator's own path already does exactly this -- @c san_alloc falls
         * back to an ordinary block when the guard cannot be served -- so
         * mirroring it is not a special case, it is the same rule.
         *
         * The block loses its guard.  That is honest degradation and it is
         * already counted and printed (`guard table full`), which is the
         * difference between a level that degrades and one that lies.
         *
         * \~spanish SIN GUARDA DISPONIBLE, PUES UN BLOQUE NORMAL -- y tiene que
         * ser asi, que costo un diagnostico averiguarlo.  La tabla de guardados
         * tiene `kGuardSlots` entradas y un bloque soltado NO devuelve nunca su
         * ranura: sus paginas se descomprometen y el rango se conserva, porque
         * eso es lo que hace que un uso-tras-liberar apunte al codigo correcto.
         * Asi que en una carga de verdad la tabla se llena -- medido aqui en
         * 498.554 rechazos y 6,4 GB de espacio de direcciones retenido -- y a
         * partir de ahi todo bloque con guarda que quiera crecer llega a esta
         * linea.
         *
         * Avisar de fallo aqui estaba MAL y es lo que mantuvo viva la caida
         * despues del primer arreglo: `realloc` contestaba nulo,
         * `pthread_key_create` lo convertia en ENOMEM, y `emutls_init` llamaba
         * a `abort`.  El propio camino del asignador ya hace exactamente esto
         * -- @c san_alloc recurre a un bloque normal cuando no se puede servir
         * con guarda --, asi que copiarlo no es un caso especial: es la misma
         * regla.
         *
         * El bloque pierde su guarda.  Eso es degradar con honestidad y ya se
         * cuenta y se imprime (`guard table full`), que es la diferencia entre
         * un nivel que degrada y uno que miente.  \~ */
        q = detail::alloc_body(san_grow(n));
        if (q == nullptr) {
            *out = nullptr; // genuinely out of memory; `p` is still valid
            return true;
        }
        vesta_memcpy(q, p, old);
        guarded_free(p, __builtin_frame_address(0));
        /* Into the shadow, which is where a plain block belongs: from here on
         * it is watched by canary and poison instead of by a page. */
        san_on_alloc(q, n, __builtin_return_address(0),
                     __builtin_frame_address(0));
        *out = q;
        return true;
    }
    /* The new block counted its own birth inside `san_alloc_guarded`; counting
     * it again here would make a growing buffer look like twice the traffic. */
    vesta_memcpy(q, p, old);
    guarded_free(p, __builtin_frame_address(0));
    *out = q;
    return true;
}

uint64_t san_guarded_blocks() noexcept {
    return g_guard_blocks.load(std::memory_order_relaxed);
}

uint64_t san_guarded_frees() noexcept {
    return g_guard_frees.load(std::memory_order_relaxed);
}

uint64_t san_guarded_by_tag(unsigned tag) noexcept {
    /* Out of range answers zero rather than reading past the table: a caller
     * asking about a purpose that does not exist gets "none of those", which is
     * true, instead of whatever byte followed. */
    if (tag >= VESTA_ALLOC_TAG_SLOTS) return 0;
    return g_guard_by_tag[tag].load(std::memory_order_relaxed);
}

uint64_t san_moved_bytes() noexcept {
    /* \~english Worked out here rather than kept as a running maximum, and on
     * purpose: a maximum updated on every allocation would be state on the hot
     * path for something only a test and a report ever read.  The depot is four
     * thousand entries; walking it once, when asked, costs nothing that matters
     * and keeps the allocation path at two additions.
     *
     * \~spanish Se saca aqui en vez de llevar un maximo al vuelo, y a
     * proposito: un maximo actualizado en cada reserva seria estado en el
     * camino caliente para algo que solo leen un test y un informe.  El
     * deposito son cuatro mil entradas; recorrerlo una vez, cuando se pregunta,
     * no cuesta nada que importe y deja el camino de reserva en dos sumas.
     * \~ */
    if (g_life == nullptr) return 0;
    uint64_t all = 0;
    for (uint32_t i = 1; i < kDepotSlots; ++i) all += g_life[i].bytes;
    return all;
}

namespace {

/* --------------------------------------------------------------------------
 *  \~english THE SAME FINDINGS, AS DATA
 *
 *  The text report is for reading and these are for querying, which are not the
 *  same job: "which sites moved more than a hundred megabytes and released
 *  everything" is one line of a spreadsheet and a scroll through prose.
 *
 *  ITS OWN FILES AND ITS OWN VARIABLE, next to the allocator's and never inside
 *  them.  `write_alloc_csv` belongs to the allocator; the allocator does not
 *  know this mode exists and must not learn.  So these are written from the
 *  checker's own exit path, into whatever directory it is pointed at -- the
 *  same one, normally, so the two sets sit side by side and join on nothing
 *  more than being in one place.
 *
 *  WHY THEY DO NOT JOIN ON A SITE ID.  The allocator keys a site by ONE return
 *  address; this keys it by a whole stack, deduplicated in the depot.  They are
 *  different keys answering different questions, and inventing a join between
 *  them would produce rows that look related and are not.  Said here so nobody
 *  tries.
 *
 *  \~spanish LOS MISMOS HALLAZGOS, COMO DATOS
 *
 *  El informe de texto es para leerlo y estos son para consultarlos, que no es
 *  el mismo trabajo: "que sitios movieron mas de cien megabytes y lo liberaron
 *  todo" es una linea de una hoja de calculo y un rato de scroll en prosa.
 *
 *  FICHEROS PROPIOS Y VARIABLE PROPIA, al lado de los del asignador y nunca
 *  dentro.  `write_alloc_csv` es del asignador; el asignador no sabe que este
 *  modo existe y no debe enterarse.  Asi que estos se escriben desde el camino
 *  de salida del propio comprobador, en el directorio al que se le apunte --
 *  el mismo, normalmente, para que los dos juegos queden uno al lado del otro
 *  sin mas union que estar en el mismo sitio.
 *
 *  POR QUE NO SE UNEN POR UN IDENTIFICADOR DE SITIO.  El asignador identifica
 *  un sitio por UNA direccion de retorno; esto lo identifica por una pila
 *  entera, deduplicada en el deposito.  Son claves distintas contestando
 *  preguntas distintas, e inventarles una union daria filas con pinta de estar
 *  relacionadas que no lo estan.  Dicho aqui para que nadie lo intente.
 * \~ ---------------------------------------------------------------------- */

/* \~english ITS OWN, AND THE COPY IS DELIBERATE.  `alloc_csv.cpp` has the same
 * two helpers, private to it.  Sharing them would mean editing the allocator so
 * that the checker can write files, and the rule here runs the other way: the
 * checker adapts, the allocator is not touched for its comfort.  Twenty lines
 * of directory-walking and quoting is the price, and it is a safe copy -- if it
 * ever drifts, the worst outcome is a file in a different shape, not a wrong
 * measurement.
 *
 * \~spanish PROPIOS, Y LA COPIA ES A PROPOSITO.  `alloc_csv.cpp` tiene los
 * mismos dos ayudantes, privados suyos.  Compartirlos significaria editar el
 * asignador para que el comprobador pueda escribir ficheros, y aqui la regla va
 * al reves: el comprobador se adapta, al asignador no se le toca por su
 * comodidad.  Veinte lineas de recorrer directorios y entrecomillar es el
 * precio, y es una copia segura -- si algun dia se separan, lo peor que sale es
 * un fichero con otra forma, no una medida equivocada.  \~ */
bool csv_ensure_dir(const char *dir) noexcept {
    char buf[1024];
    const size_t n = std::strlen(dir);
    if (n == 0 || n >= sizeof(buf)) return false;
    std::memcpy(buf, dir, n + 1);
    for (size_t i = 0; i <= n; ++i) {
        if (buf[i] != '/' && buf[i] != '\\' && buf[i] != '\0') continue;
        /* Neither the leading separator of an absolute path nor the `C:` of a
         * drive is a directory anybody can create. */
        if (i == 0 || (i == 2 && buf[1] == ':')) continue;
        const char saved = buf[i];
        buf[i] = '\0';
#if defined(_WIN32)
        const bool ok = CreateDirectoryA(buf, nullptr) != 0 ||
                        GetLastError() == ERROR_ALREADY_EXISTS;
#else
        const bool ok = ::mkdir(buf, 0777) == 0 || errno == EEXIST;
#endif
        buf[i] = saved;
        if (!ok) return false;
    }
    return true;
}

/// A field that may hold a comma, a quote or a newline.  Empty when null, which
/// a table reads as "not known" and not as an empty name.
void csv_field(FILE *f, const char *s) noexcept {
    if (s == nullptr) return;
    bool needs = false;
    for (const char *p = s; *p != '\0'; ++p)
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') needs = true;
    if (!needs) {
        std::fputs(s, f);
        return;
    }
    std::fputc('"', f);
    for (const char *p = s; *p != '\0'; ++p) {
        if (*p == '"') std::fputc('"', f);
        std::fputc(*p, f);
    }
    std::fputc('"', f);
}

/// Opens `<dir>/<name>`.  Null if it cannot, having said so.
FILE *csv_open(const char *dir, const char *name) noexcept {
    char path[1024];
    const int n = std::snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (n <= 0 || size_t(n) >= sizeof(path)) {
        std::fprintf(stderr, "[allocator/check] CSV: path too long for %s\n",
                     name);
        return nullptr;
    }
    FILE *f = std::fopen(path, "wb");
    if (f == nullptr)
        std::fprintf(stderr,
                     "[allocator/check] CSV: could not write %s -- does the "
                     "directory exist?\n",
                     path);
    return f;
}

/// One line of the leak report: a stack and what is still hanging off it.
struct Leak {
    uint32_t stack;
    uint32_t blocks;
    uint64_t bytes;
};

/// \~english Sorted by bytes and then by stack id, which is what makes two runs
///           of the same program print the same list.  A report that reorders
///           itself between runs cannot gate a build, and gating a build is the
///           point.
/// \~spanish Ordenado por bytes y luego por identificador de pila, que es lo
///           que hace que dos corridas del mismo programa impriman la misma
///           lista.  Un informe que se reordena entre corridas no puede cortar
///           un build, y cortar un build es de lo que se trata.
/// \~
void sort_leaks(Leak *v, uint32_t n) noexcept {
    for (uint32_t i = 1; i < n; ++i) {
        const Leak k = v[i];
        uint32_t j = i;
        while (j > 0 && (v[j - 1].bytes < k.bytes ||
                         (v[j - 1].bytes == k.bytes &&
                          v[j - 1].stack > k.stack))) {
            v[j] = v[j - 1];
            --j;
        }
        v[j] = k;
    }
}

/**
 * @brief
 * \~english Writes what the checker measured, as two tables.
 * \~spanish Escribe lo que midio el comprobador, en dos tablas.
 * \~
 *
 * \~english
 * `check_sites.csv` is one row per stack: how much it moved, how much it left
 * behind, and what its blocks turned out to BE.  `check_frames.csv` is one row
 * per frame of each stack, joined on `stack_id`, so the shape of a site can be
 * asked for without parsing prose.
 *
 * THE TWO SIDES OF EVERY SITE ARE IN ONE ROW, and that is the point of writing
 * it rather than reading it: `bytes` counts every block the site ever handed
 * out and `alive_bytes` only what survived.  A site with a huge first column
 * and a zero second is a buffer that grew and was released -- gigabytes moved,
 * nothing leaked, invisible to every other figure here, and the one shape that
 * decides a peak.  Sorting a spreadsheet by that ratio finds them; reading a
 * report does not.
 *
 * \~spanish
 * `check_sites.csv` es una fila por pila: cuanto movio, cuanto dejo detras, y
 * que resultaron SER sus bloques.  `check_frames.csv` es una fila por marco de
 * cada pila, unidas por `stack_id`, para poder preguntar por la forma de un
 * sitio sin analizar prosa.
 *
 * LAS DOS CARAS DE CADA SITIO VAN EN UNA FILA, y de eso se trata al escribirlo
 * en vez de leerlo: `bytes` cuenta todo bloque que el sitio entrego alguna vez
 * y `alive_bytes` solo lo que sobrevivio.  Un sitio con la primera columna
 * enorme y la segunda a cero es un buffer que crecio y se libero -- gigabytes
 * movidos, cero fugado, invisible para todas las demas cifras de aqui, y la
 * unica forma que decide un pico.  Ordenar una hoja por esa razon los
 * encuentra; leer un informe no.
 * \~
 *
 * @param dir
 * \~english where to put them; created if it is not there.
 * \~spanish donde ponerlos; se crea si no esta.
 * \~
 * @return
 * \~english false if nothing could be written, having said why.
 * \~spanish false si no se pudo escribir nada, habiendo dicho por que.
 * \~
 */
bool write_check_csv(const char *dir) noexcept {
    if (dir == nullptr || dir[0] == '\0') return false;
    if (g_life == nullptr || g_depot == nullptr) return false;
    if (!csv_ensure_dir(dir)) {
        std::fprintf(stderr,
                     "[allocator/check] CSV: could not create %s\n", dir);
        return false;
    }

    /* \~english What is still alive, per stack.  Counted here rather than taken
     * from the report so this can be called on its own -- a table that only
     * exists as a side effect of printing is a table nobody can ask for.
     *
     * \~spanish Lo que sigue vivo, por pila.  Se cuenta aqui en vez de cogerlo
     * del informe para que esto se pueda llamar solo -- una tabla que solo
     * existe como efecto secundario de imprimir es una tabla que nadie puede
     * pedir.  \~ */
    const size_t abytes = sizeof(Leak) * kDepotSlots;
    void *amem = os_alloc(abytes, kOsReadWrite);
    if (amem == nullptr) return false;
    Leak *alive = static_cast<Leak *>(amem);
    std::memset(amem, 0, abytes);

    if (g_rows != nullptr) {
        for (uint32_t r = 0; r < g_row_count; ++r) {
            Slot *row = g_rows[r].load(std::memory_order_acquire);
            if (row == nullptr) continue;
            for (uint32_t i = 0; i < kSlotsPerChunk; ++i) {
                if (meta_state(row[i].meta) != kStAlive) continue;
                const uint32_t id = row[i].alloc_stack;
                if (id < kDepotSlots) {
                    alive[id].blocks++;
                    alive[id].bytes += row[i].req;
                }
            }
        }
    }

    /* \~english AND THE GUARDED ONES, which live in a table of their own.  At
     * the guard level a block goes on pages outside the region, so the shadow
     * above never saw it: counting only the shadow would make `alive_bytes`
     * fall as the level RISES, which reads as "the strictest mode found less"
     * instead of "this table does not cover that mode".  Below that level the
     * table is empty and this loop is one comparison.
     *
     * \~spanish Y LOS QUE TIENEN GUARDA, que viven en una tabla propia.  En el
     * nivel de guarda un bloque va a paginas fuera de la region, asi que el
     * sombreado de arriba no lo vio nunca: contar solo el sombreado haria que
     * `alive_bytes` bajara al SUBIR el nivel, que se lee como "el modo mas
     * estricto encontro menos" en vez de "esta tabla no cubre ese modo".  Por
     * debajo de ese nivel la tabla esta vacia y este bucle es una comparacion.
     * \~ */
    if (g_guarded != nullptr) {
        for (uint32_t i = 0; i < kGuardSlots; ++i) {
            if (g_guarded[i].p.load(std::memory_order_acquire) == nullptr)
                continue;
            if (g_guarded[i].state != kStAlive) continue;
            const uint32_t id = g_guarded[i].alloc_stack;
            if (id < kDepotSlots) {
                alive[id].blocks++;
                alive[id].bytes += uint64_t(g_guarded[i].req);
            }
        }
    }

    bool any = false;

    if (FILE *f = csv_open(dir, "check_sites.csv")) {
        std::fprintf(f,
                     "stack_id,births,bytes,outside_births,outside_bytes,"
                     "deaths,alive_blocks,alive_bytes,life_avg,life_max,"
                     "cross_thread,size_min,size_max,use,shape,walked,frames\n");
        for (uint32_t i = 1; i < kDepotSlots; ++i) {
            const Life &l = g_life[i];
            /* A stack with no births and nothing alive was never a site. */
            if (l.births == 0 && alive[i].blocks == 0) continue;
            const Stack &s = g_depot[i];
            std::fprintf(f,
                         "%u,%llu,%llu,%llu,%llu,%llu,%u,%llu,%llu,%u,%llu,%u,"
                         "%u,%s,%s,%d,%u\n",
                         i, (unsigned long long)l.births,
                         (unsigned long long)l.bytes,
                         (unsigned long long)l.outside_births,
                         (unsigned long long)l.outside_bytes,
                         (unsigned long long)l.deaths, alive[i].blocks,
                         (unsigned long long)alive[i].bytes,
                         (unsigned long long)(l.deaths != 0
                                                  ? l.life_sum / l.deaths
                                                  : 0),
                         l.life_max, (unsigned long long)l.unknown, l.size_min,
                         l.size_max, use_word(l), shape_word(l),
                         s.walked ? 1 : 0, unsigned(s.frames));
        }
        std::fclose(f);
        any = true;
    }

    /* \~english THE SAME COLUMNS AS THE ALLOCATOR'S `frames.csv`, on purpose:
     * whoever already has a query for one should not have to write a second one
     * for this.  `frame` is the call frame -- what the stack walk found -- and
     * `depth` the inline level inside it, which are two different axes that get
     * confused when a table calls them both "depth".
     *
     * \~spanish LAS MISMAS COLUMNAS QUE EL `frames.csv` DEL ASIGNADOR, a
     * proposito: quien ya tenga una consulta para uno no deberia tener que
     * escribir otra para esto.  `frame` es el marco de llamada -- lo que
     * encontro el recorrido de pila -- y `depth` el nivel de inline dentro de
     * el, que son dos ejes distintos y se confunden cuando una tabla llama
     * "depth" a los dos.  \~ */
    if (FILE *f = csv_open(dir, "check_frames.csv")) {
        std::fprintf(f, "stack_id,frame,depth,inlined,function,file,line\n");
        for (uint32_t i = 1; i < kDepotSlots; ++i) {
            if (g_life[i].births == 0 && alive[i].blocks == 0) continue;
            const Stack &s = g_depot[i];
            for (unsigned k = 0; k < s.frames; ++k) {
                AllocFrame fr[kNameFrames];
                const unsigned got = name_of(s.pc[k], fr, kNameFrames);
                if (got == 0) {
                    /* Nobody could resolve it, so the address is written as
                     * what it is.  An empty row would read as "no code here". */
                    char buf[512];
                    raw_name(s.pc[k], buf, sizeof buf);
                    std::fprintf(f, "%u,%u,0,0,", i, k);
                    csv_field(f, buf);
                    std::fprintf(f, ",,0\n");
                    continue;
                }
                for (unsigned d = 0; d < got; ++d) {
                    std::fprintf(f, "%u,%u,%u,%d,", i, k, d,
                                 fr[d].inlined ? 1 : 0);
                    csv_field(f, alloc_readable_name(fr[d].function));
                    std::fputc(',', f);
                    csv_field(f, fr[d].file);
                    std::fprintf(f, ",%u\n", fr[d].line);
                }
            }
        }
        std::fclose(f);
        any = true;
    }

    /* \~english AND THE BLIND SPOTS, in their own file, because a table that
     * does not carry them reads as complete.  The depot has a ceiling and the
     * blocks past it belong to no row above.
     *
     * \~spanish Y LOS PUNTOS CIEGOS, en su propio fichero, porque una tabla que
     * no los lleva se lee como completa.  El deposito tiene techo y los bloques
     * pasados de ahi no son de ninguna fila de arriba.  \~ */
    if (FILE *f = csv_open(dir, "check_summary.csv")) {
        std::fprintf(f, "key,value\n");
        std::fprintf(f, "depot_slots,%u\n", kDepotSlots);
        /* What THIS mode served, which the allocator's own counters cannot see:
         * `site entries == allocator served + this`.  See `g_guard_blocks`. */
        std::fprintf(f, "blocks_served_by_the_checker,%llu\n",
                     (unsigned long long)g_guard_blocks.load(
                         std::memory_order_relaxed));
        std::fprintf(f, "bytes_reserved_for_guards,%llu\n",
                     (unsigned long long)g_guard_bytes.load(
                         std::memory_order_relaxed));
        std::fprintf(f, "stacks_that_did_not_fit,%llu\n",
                     (unsigned long long)g_depot_full.load(
                         std::memory_order_relaxed));
        std::fprintf(f, "blocks_with_no_site,%llu\n",
                     (unsigned long long)g_birth_lost);
        std::fprintf(f, "bytes_with_no_site,%llu\n",
                     (unsigned long long)g_birth_lost_bytes);
        std::fprintf(f, "blocks_outside_the_shadow,%llu\n",
                     (unsigned long long)g_uncovered.load(
                         std::memory_order_relaxed));
        /* What writing this cost, kept out of every figure above.  A checker
         * that hides its own weight is one you cannot subtract. */
        std::fprintf(f, "blocks_the_report_itself_made,%llu\n",
                     (unsigned long long)g_report_births);
        std::fprintf(f, "bytes_the_report_itself_made,%llu\n",
                     (unsigned long long)g_report_bytes);
        std::fprintf(f, "level,%u\n", unsigned(detail::g_san_level));
        /* \~english The depth ASKED FOR, not the ceiling: it is what decides
         * how many distinct stacks there are, so it is what a saturated depot
         * has to be read against.
         *
         * \~spanish La profundidad PEDIDA, no el techo: es lo que decide
         * cuantas pilas distintas hay, asi que es contra lo que hay que leer un
         * deposito saturado.  \~ */
        std::fprintf(f, "frames_walked,%u\n", g_depth);
        std::fprintf(f, "frames_per_stack_max,%u\n", kFrames);
        std::fclose(f);
        any = true;
    }

    os_free(amem, abytes);
    return any;
}

/**
 * @brief
 * \~english Walks the shadow at exit and says what never came back.
 * \~spanish Recorre el sombreado al salir y dice que no volvio nunca.
 * \~
 *
 * \~english
 * WHY THE MODULE IS ASKED HERE AND NOT WHEN ALLOCATING: because the allocator
 * must not pay for the checker's comfort.  The shadow keeps a raw address; who
 * it belongs to is worked out ONCE, at the end, and only for the blocks that
 * survived.  It is also what replaces a suppression list -- third-party
 * libraries leak on purpose, and a list of exceptions is how a checker starts
 * lying, so the split comes out DERIVED from where the code lives.
 *
 * \~spanish
 * POR QUE EL MODULO SE PREGUNTA AQUI Y NO AL RESERVAR: porque el asignador no
 * puede pagar la comodidad del comprobador.  El sombreado guarda una direccion
 * cruda; de quien es se averigua UNA vez, al final, y solo por los bloques que
 * sobrevivieron.  Es ademas lo que sustituye a una lista de supresiones -- las
 * librerias de terceros sueltan cosas a proposito, y una lista de excepciones
 * es como un comprobador empieza a mentir, asi que el reparto sale DERIVADO de
 * donde vive el codigo.
 * \~
 */
void report() noexcept {
    /* \~english THE DEPOT IS WHAT THIS NEEDS; the shadow is not.
     *
     * This used to return unless BOTH were up, and the consequence was the
     * worst one a checker can have: at the guard level the shadow is never
     * built -- blocks come out of pages of their own and never touch it -- so
     * the strictest setting printed NOTHING AT ALL.  Not a short report, not a
     * warning: an empty stderr, with the run looking exactly like a clean one.
     * Measured in the compiler itself, `g_rows` is a live table of 4.194.304
     * rows at the poison level and a null pointer at the guard level, and
     * eleven thousand guarded blocks had been served and counted with nobody
     * left to print them.
     *
     * What the shadow holds is leaks and lives.  Everything else -- what each
     * site moved, who gives back what, the guarded blocks, the verdicts already
     * printed as they happened -- lives elsewhere and is worth saying on its
     * own.  So the missing half is NAMED and the rest goes out.
     *
     * \~spanish LO QUE ESTO NECESITA ES EL DEPOSITO; el sombreado no.
     *
     * Antes volvia si no estaban los dos, y la consecuencia era la peor que
     * puede tener un comprobador: en el nivel de guarda el sombreado no se monta
     * nunca -- los bloques salen de paginas propias y no lo tocan --, asi que el
     * ajuste mas estricto no imprimia NADA.  Ni un informe corto ni un aviso:
     * un stderr vacio, con la corrida con el mismo aspecto que una limpia.
     * Medido en el propio compilador, `g_rows` es una tabla viva de 4.194.304
     * filas en el nivel de veneno y un puntero nulo en el de guarda, y once mil
     * bloques con guarda se habian servido y contado sin que quedara nadie para
     * imprimirlos.
     *
     * Lo que guarda el sombreado son fugas y vidas.  Todo lo demas -- lo que
     * movio cada sitio, quien devuelve lo de quien, los bloques con guarda, los
     * veredictos ya impresos segun ocurrian -- vive en otro sitio y merece
     * decirse igual.  Asi que la mitad que falta se NOMBRA y el resto sale.  \~
     */
    if (g_depot == nullptr) return;
    if (g_rows == nullptr)
        std::fprintf(stderr,
                     "[allocator/check] NOT COVERED: the shadow is not up, so "
                     "there are no leaks and no lives below -- at the guard "
                     "level blocks come out of pages of their own and never "
                     "reach it.  What follows is everything that does NOT come "
                     "from the shadow; it is not a clean run, it is a shorter "
                     "report\n");
    /* \~english FROM HERE ON, WHAT WE ALLOCATE IS OURS.  Everything below
     * resolves names, and resolving allocates; without this the report would
     * appear in its own figures.  See `g_in_report`.
     *
     * \~spanish DE AQUI EN ADELANTE, LO QUE RESERVEMOS ES NUESTRO.  Todo lo de
     * abajo resuelve nombres, y resolver reserva; sin esto el informe saldria
     * en sus propias cifras.  Ver `g_in_report`.  \~ */
    g_in_report = true;

    /* One entry per stack, indexed by its id: the depot is small and this way
     * grouping is a single pass with no table of its own. */
    const size_t bytes = sizeof(Leak) * kDepotSlots;
    void *mem = os_alloc(bytes, kOsReadWrite);
    if (mem == nullptr) return;
    Leak *by_stack = static_cast<Leak *>(mem);
    std::memset(mem, 0, bytes);

    uint64_t alive = 0, alive_bytes = 0;
    for (uint32_t r = 0; r < g_row_count; ++r) {
        Slot *row = g_rows[r].load(std::memory_order_acquire);
        if (row == nullptr) continue;
        for (uint32_t i = 0; i < kSlotsPerChunk; ++i) {
            if (meta_state(row[i].meta) != kStAlive) continue;
            ++alive;
            alive_bytes += row[i].req;
            const uint32_t id = row[i].alloc_stack;
            if (id < kDepotSlots) {
                by_stack[id].stack = id;
                by_stack[id].blocks++;
                by_stack[id].bytes += row[i].req;
            }
        }
    }

    std::fprintf(stderr, "\n[allocator/check] level %u, poison %u, guard %u\n",
                 unsigned(detail::g_san_level), unsigned(g_poison),
                 unsigned(g_guard_edge));

    /* \~english WHAT IT DID NOT LOOK AT, and it goes FIRST.  A checker that
     * stays quiet about its blind spots is read as "there is nothing there",
     * which is the one way this whole thing could do harm.
     *
     * \~spanish LO QUE NO MIRO, y va PRIMERO.  Un comprobador que se calla sus
     * puntos ciegos se lee como "ahi no hay nada", que es la unica forma en que
     * todo esto podria hacer dano.  \~ */
    const uint64_t unc = g_uncovered.load(std::memory_order_relaxed);
    if (unc != 0)
        std::fprintf(stderr,
                     "[allocator/check] NOT COVERED: %llu operations on blocks "
                     "outside the small-class region (spans, big classes, "
                     "direct blocks over 16 MiB)\n",
                     (unsigned long long)unc);
    const uint64_t full = g_depot_full.load(std::memory_order_relaxed);
    if (full != 0)
        std::fprintf(stderr,
                     "[allocator/check] NOT COVERED: %llu stacks did not fit "
                     "in the depot of %u and were dropped whole, never merged "
                     "with somebody else's\n",
                     (unsigned long long)full, kDepotSlots);
    const uint64_t norow = g_no_shadow.load(std::memory_order_relaxed);
    if (norow != 0)
        std::fprintf(stderr,
                     "[allocator/check] NOT COVERED: %llu chunks the system "
                     "would not give shadow memory for\n",
                     (unsigned long long)norow);

    const uint64_t gfull = g_guard_full.load(std::memory_order_relaxed);
    if (gfull != 0)
        std::fprintf(stderr,
                     "[allocator/check] NOT COVERED: %llu allocations could "
                     "not get a guarded block -- the table of %u was full -- "
                     "and were served the ordinary way\n",
                     (unsigned long long)gfull, kGuardSlots);
    const uint64_t gb = g_guard_bytes.load(std::memory_order_relaxed);
    const uint64_t gblocks = g_guard_blocks.load(std::memory_order_relaxed);
    if (gb != 0)
        std::fprintf(stderr,
                     "[allocator/check] %llu MiB of address space went to "
                     "guarded blocks and is NOT coming back: that is what buys "
                     "the fault happening where the mistake is\n",
                     (unsigned long long)(gb / (1024 * 1024)));
    /* \~english WHO SERVED WHAT, said plainly.  The allocator's own counters
     * only ever see what IT carved out of a region, so at this level most of a
     * run is missing from them -- and that is not a discrepancy to excuse, it
     * is the warning that this run's memory behaviour is not the program's
     * usual one.  Whoever reads a profile taken here has to know that before
     * drawing anything from it.
     *
     * \~spanish QUIEN SIRVIO QUE, dicho claro.  Los contadores del propio
     * asignador solo ven lo que EL recorto de una region, asi que en este nivel
     * la mayor parte de una corrida les falta -- y eso no es una discrepancia
     * que disculpar, es el aviso de que el comportamiento de memoria de esta
     * corrida no es el habitual del programa.  Quien lea un perfil tomado aqui
     * tiene que saberlo antes de sacar nada de el.  \~ */
    if (gblocks != 0)
        std::fprintf(stderr,
                     "[allocator/check] %llu blocks were served BY THIS MODE, "
                     "out of its own pages -- the allocator did not carve them "
                     "and does not count them, so a profile taken here is not "
                     "this program's usual one\n",
                     (unsigned long long)gblocks);

    const uint64_t cross = g_cross_thread.load(std::memory_order_relaxed);
    if (cross != 0)
        std::fprintf(stderr,
                     "[allocator/check] %llu blocks were allocated on one "
                     "thread and released on another -- legal, and worth "
                     "seeing\n",
                     (unsigned long long)cross);

    /* \~english WHAT THE SITES ARE, measured.  This is the half of the report
     * that is not about mistakes: it says, for every place that allocates, how
     * long its blocks lived and whether they were all the same size -- which is
     * exactly the two axes a caller can declare.  Declaring then stops being
     * the source of truth and becomes a claim with something to check it
     * against.
     *
     * \~spanish LO QUE SON LOS SITIOS, medido.  Esta es la mitad del informe
     * que no va de fallos: dice, por cada sitio que reserva, cuanto vivieron
     * sus bloques y si eran todos del mismo tamano -- que son justo los dos
     * ejes que quien llama puede declarar.  Declarar deja entonces de ser la
     * fuente de verdad y pasa a ser una afirmacion con algo contra lo que
     * contrastarla.  \~ */
    /* \~english WHAT MOVED THE MOST, which is a different question from what
     * leaked and was not answerable until now.  A buffer that doubles twenty
     * times and is released moves gigabytes and leaves nothing behind: every
     * other figure in this report is blind to it, and it is the shape that
     * decides a peak.  Sorted by bytes and capped, with the remainder said
     * rather than dropped.
     *
     * \~spanish LO QUE MAS MOVIO, que es una pregunta distinta de lo que se
     * fugo y no se podia contestar hasta ahora.  Un buffer que se duplica
     * veinte veces y se libera mueve gigabytes y no deja nada detras: todas las
     * demas cifras de este informe son ciegas para el, y es la forma que decide
     * un pico.  Ordenado por bytes y con tope, y lo que queda fuera se dice en
     * vez de perderse.  \~ */
    if (g_life != nullptr) {
        /* \~english On the stack and not allocated: twenty entries kept in
         * order beats a table of sixty-five thousand, and the report must not
         * need memory to say where the memory went.
         *
         * \~spanish En la pila y sin reservar: veinte entradas mantenidas en
         * orden salen mejor que una tabla de sesenta y cinco mil, y el informe
         * no puede necesitar memoria para decir donde se fue la memoria.  \~ */
        constexpr uint32_t kTop = 20;
        Leak top[kTop];
        uint32_t kept = 0;
        uint32_t movers = 0;
        uint64_t moved = 0, moved_blocks = 0;
        uint64_t out_bytes = 0, out_blocks = 0;

        for (uint32_t i = 1; i < kDepotSlots; ++i) {
            const Life &l = g_life[i];
            if (l.births == 0) continue;
            ++movers;
            moved += l.bytes;
            moved_blocks += l.births;
            out_bytes += l.outside_bytes;
            out_blocks += l.outside_births;

            if (kept == kTop && l.bytes <= top[kept - 1].bytes) continue;
            const Leak e = {i, uint32_t(l.births), l.bytes};
            uint32_t j = kept < kTop ? kept : kTop - 1;
            while (j > 0 && (top[j - 1].bytes < e.bytes ||
                             (top[j - 1].bytes == e.bytes &&
                              top[j - 1].stack > e.stack))) {
                top[j] = top[j - 1];
                --j;
            }
            top[j] = e;
            if (kept < kTop) ++kept;
        }

        if (movers != 0) {
            std::fprintf(stderr,
                         "\n[allocator/check] what MOVED the most: %llu bytes "
                         "in %llu blocks from %u sites, over the whole run.  "
                         "This counts every block, released or not -- which is "
                         "what the leak list above cannot see.\n",
                         (unsigned long long)moved,
                         (unsigned long long)moved_blocks, movers);
            /* \~english HOW MUCH OF IT WAS ONLY COUNTED, said right under the
             * total instead of left to the blind-spot line further up.  A
             * reader who sees "what moved the most" and a figure will take the
             * figure as the program's traffic; if most of it came from blocks
             * this checker could only weigh and never inspect, that changes
             * what the list below is worth, and it has to be said in the same
             * breath as the number.
             *
             * \~spanish CUANTO DE ESO SOLO SE CONTO, dicho justo debajo del
             * total en vez de dejarlo a la linea de puntos ciegos de mas
             * arriba.  Quien lea "lo que mas movio" y una cifra la tomara por
             * el trafico del programa; si la mayor parte vino de bloques que
             * este comprobador solo pudo pesar y nunca mirar, eso cambia lo que
             * vale la lista de abajo, y hay que decirlo en la misma frase que
             * el numero.  \~ */
            if (out_bytes != 0)
                std::fprintf(stderr,
                             "                  of which %llu bytes in %llu "
                             "blocks were only WEIGHED, not inspected: they "
                             "are over the small-class limit, so they have no "
                             "shadow slot and therefore no life, no shape and "
                             "no leak verdict\n",
                             (unsigned long long)out_bytes,
                             (unsigned long long)out_blocks);
            uint64_t shown = 0;
            for (uint32_t i = 0; i < kept; ++i) {
                shown += top[i].bytes;
                const Life &l = g_life[top[i].stack];
                std::fprintf(stderr, "\n  %llu bytes in %u allocs (avg %llu)",
                             (unsigned long long)top[i].bytes, top[i].blocks,
                             (unsigned long long)(top[i].blocks != 0
                                                      ? top[i].bytes /
                                                            top[i].blocks
                                                      : 0));
                /* \~english Per site as well as in the total: a vector that
                 * doubles crosses the boundary partway through, so the same
                 * site is on both sides and only its own numbers show where.
                 *
                 * \~spanish Por sitio ademas de en el total: un vector que se
                 * duplica cruza la frontera a mitad de camino, asi que el mismo
                 * sitio esta a los dos lados y solo sus propias cifras dicen
                 * donde.  \~ */
                if (l.outside_bytes != 0)
                    std::fprintf(stderr, "  | %llu bytes in %llu blocks only weighed",
                                 (unsigned long long)l.outside_bytes,
                                 (unsigned long long)l.outside_births);
                std::fputc('\n', stderr);
                print_stack("  allocated", top[i].stack);
            }
            /* \~english THE REST IS SAID, not dropped.  A capped list that does
             * not admit what it left out reads as the whole answer.
             *
             * \~spanish EL RESTO SE DICE, no se tira.  Una lista con tope que
             * no confiesa lo que dejo fuera se lee como la respuesta entera.
             * \~ */
            if (movers > kept)
                std::fprintf(stderr,
                             "\n  and %u more sites, %llu bytes between them, "
                             "not listed\n",
                             movers - kept, (unsigned long long)(moved - shown));
            if (g_birth_lost != 0)
                std::fprintf(stderr,
                             "  and %llu blocks (%llu bytes) whose stack did "
                             "not fit in the depot, so they belong to no site "
                             "here and are NOT in the total above\n",
                             (unsigned long long)g_birth_lost,
                             (unsigned long long)g_birth_lost_bytes);
        }
    }

    if (g_life != nullptr) {
        uint32_t sites = 0;
        for (uint32_t i = 1; i < kDepotSlots; ++i)
            if (g_life[i].deaths != 0 || g_life[i].unknown != 0) ++sites;
        if (sites != 0) {
            std::fprintf(stderr,
                         "\n[allocator/check] what %u sites turned out to BE, "
                         "measured (not declared).  A life is counted in "
                         "allocations of its own thread, never in time, so two "
                         "runs give the same number: under %u is Instant, under "
                         "%u Medium, above that Long.\n",
                         sites, g_life_instant, g_life_medium);
            for (uint32_t i = 1; i < kDepotSlots; ++i) {
                const Life &l = g_life[i];
                if (l.deaths == 0 && l.unknown == 0) continue;
                std::fprintf(stderr,
                             "\n  %s / %s  -- %llu blocks, life avg %llu, max "
                             "%u, sizes %u..%u",
                             use_word(l), shape_word(l),
                             (unsigned long long)l.deaths,
                             (unsigned long long)(l.deaths != 0
                                                      ? l.life_sum / l.deaths
                                                      : 0),
                             l.life_max, l.size_min, l.size_max);
                if (l.unknown != 0)
                    std::fprintf(stderr,
                                 "  | %llu released on another thread, whose "
                                 "lives are not comparable and are NOT in the "
                                 "average",
                                 (unsigned long long)l.unknown);
                std::fprintf(stderr, "\n");
                print_stack("  allocated", i);
            }
        }
    }

    /* \~english WHO OWNS WHAT, which is the one question the per-site figures
     * above cannot reach.  They say how much a place allocates and how long its
     * blocks live; none of them says who ends up responsible.
     *
     * The sites are listed by how many DIFFERENT places give their blocks back,
     * smallest first, because that is the order in which the answer is useful:
     * one place means the site has an owner and could be sent to an arena of
     * its own; several mean it is shared and belongs where it is.  The rows are
     * evidence for that decision, not the decision.
     *
     * \~spanish DE QUIEN ES CADA COSA, que es la unica pregunta a la que las
     * cifras por sitio de arriba no llegan.  Dicen cuanto reserva un sitio y
     * cuanto viven sus bloques; ninguna dice quien acaba respondiendo.
     *
     * Los sitios salen ordenados por CUANTOS sitios distintos devuelven sus
     * bloques, de menos a mas, porque ese es el orden en que la respuesta sirve:
     * uno solo quiere decir que el sitio tiene dueño y podria ir a una arena
     * propia; varios, que es compartido y su lugar es donde esta.  Las filas son
     * la prueba para esa decision, no la decision.  \~ */
    if (g_pairs != nullptr) {
        uint32_t used = 0;
        for (uint32_t i = 0; i < kPairSlots; ++i)
            if (g_pairs[i].key.load(std::memory_order_relaxed) != 0) ++used;
        if (used != 0) {
            std::fprintf(stderr,
                         "\n[allocator/check] WHO GIVES BACK WHAT: %u pairs of "
                         "(site that handed out, site that gave back).  A site "
                         "whose blocks all come back through ONE place has an "
                         "owner and could go to an arena of its own; one whose "
                         "blocks come back through several is shared and "
                         "belongs on the common path.  Not being able to show "
                         "a single owner is NOT showing there are several.\n",
                         used);
            /* \~english ONE PASS, grouping into a row per allocating site --
             * the same shape the leak list above already uses, and for the same
             * reason.  Asking the table once per site instead would be
             * `kDepotSlots * kPairSlots`, four thousand million reads to print
             * a page, which is not a slow report but a hung process.  The cost
             * is decided here, before writing it.
             *
             * \~spanish UNA PASADA, agrupando en una fila por sitio de reserva
             * -- la misma forma que ya usa la lista de fugas de arriba, y por la
             * misma razon.  Preguntarle a la tabla una vez por sitio seria
             * `kDepotSlots * kPairSlots`, cuatro mil millones de lecturas para
             * imprimir una pagina, que no es un informe lento sino un proceso
             * colgado.  El coste se decide aqui, antes de escribirlo.  \~ */
            const size_t own_bytes = sizeof(Owned) * kDepotSlots;
            void *own_mem = os_alloc(own_bytes, kOsReadWrite);
            if (own_mem != nullptr) {
                Owned *by_site = static_cast<Owned *>(own_mem);
                std::memset(own_mem, 0, own_bytes);
                for (uint32_t i = 0; i < kPairSlots; ++i) {
                    const uint64_t k =
                        g_pairs[i].key.load(std::memory_order_relaxed);
                    if (k == 0) continue;
                    const uint32_t a = uint32_t(k >> 32);
                    if (a >= kDepotSlots) continue;
                    Owned &o = by_site[a];
                    ++o.owners;
                    o.one_owner = uint32_t(k & 0xFFFFFFFFu);
                    o.blocks += g_pairs[i].blocks;
                    o.bytes += g_pairs[i].bytes;
                }
                for (uint32_t a = 1; a < kDepotSlots; ++a) {
                    const Owned &o = by_site[a];
                    if (o.owners == 0) continue;
                    if (o.owners == 1)
                        std::fprintf(stderr,
                                     "\n  ONE OWNER -- %llu blocks, %llu "
                                     "bytes\n",
                                     (unsigned long long)o.blocks,
                                     (unsigned long long)o.bytes);
                    else
                        std::fprintf(stderr,
                                     "\n  SHARED between %u places -- %llu "
                                     "blocks, %llu bytes\n",
                                     o.owners, (unsigned long long)o.blocks,
                                     (unsigned long long)o.bytes);
                    print_stack("  allocated", a);
                    if (o.owners == 1) print_stack("  released", o.one_owner);
                }
                os_free(own_mem, own_bytes);
            }
        }
        const uint64_t pfull = g_pair_full.load(std::memory_order_relaxed);
        if (pfull != 0)
            std::fprintf(stderr,
                         "[allocator/check] NOT COVERED: %llu releases whose "
                         "pair found no room in the table of %u, so the "
                         "ownership above is what FIT, not what happened\n",
                         (unsigned long long)pfull, kPairSlots);
    }

    if (alive == 0) {
        /* \~english AND WHICH OF THE TWO ZEROES THIS IS.  With no shadow
         * nothing was ever watched, so "nothing was left alive" would be a
         * claim about memory when it is a fact about the checker -- the exact
         * shape of lie this library exists to prevent.
         *
         * \~spanish Y CUAL DE LOS DOS CEROS ES ESTE.  Sin sombreado no se
         * vigilo nada, asi que "no quedo nada vivo" seria una afirmacion sobre
         * la memoria cuando es un hecho sobre el comprobador -- justo la forma
         * de mentira que esta libreria existe para impedir.  \~ */
        if (g_rows == nullptr)
            std::fprintf(stderr,
                         "[allocator/check] whether anything was left alive is "
                         "UNKNOWN here: the shadow that would have watched it "
                         "was never built\n");
        else
            std::fprintf(stderr, "[allocator/check] nothing was left alive\n");
    } else {
        uint32_t n = 0;
        for (uint32_t i = 0; i < kDepotSlots; ++i)
            if (by_stack[i].blocks != 0) by_stack[n++] = by_stack[i];
        sort_leaks(by_stack, n);

        std::fprintf(stderr,
                     "\n[allocator/check] %s: %llu blocks (%llu bytes) were "
                     "never released, from %u places\n",
                     certainty_word(Certainty::Suspected),
                     (unsigned long long)alive, (unsigned long long)alive_bytes,
                     n);
        std::fprintf(stderr,
                     "                  SUSPECTED and not proven: memory a "
                     "program keeps until it exits looks exactly like this\n");
        for (uint32_t i = 0; i < n; ++i) {
            std::fprintf(stderr, "\n  %u blocks, %llu bytes\n",
                         by_stack[i].blocks,
                         (unsigned long long)by_stack[i].bytes);
            print_stack("  allocated", by_stack[i].stack);
        }
        g_verdicts.fetch_add(1, std::memory_order_relaxed);
    }

    os_free(mem, bytes);

    /* \~english AND THE SAME THING AS DATA, if a directory was named.  Its own
     * variable, next to the level's: whoever wants tables asks for tables, and
     * a run that did not ask writes nothing.  It goes AFTER the text so a
     * failure to write says so underneath a report that already came out whole.
     *
     * \~spanish Y LO MISMO COMO DATOS, si se nombro un directorio.  Variable
     * propia, al lado de la del nivel: quien quiera tablas pide tablas, y una
     * corrida que no las pidio no escribe nada.  Va DESPUES del texto para que
     * un fallo al escribir se diga debajo de un informe que ya salio entero.
     * \~ */
    char dir[512];
    const size_t dn = os_env("VESTA_ALLOC_SAN_CSV", dir, sizeof dir);
    if (dn != kOsEnvUnset && dn != 0 && dir[0] != '\0') {
        if (write_check_csv(dir))
            std::fprintf(stderr,
                         "\n[allocator/check] tables written to %s "
                         "(check_sites.csv, check_frames.csv, "
                         "check_summary.csv)\n",
                         dir);
        else
            std::fprintf(stderr,
                         "\n[allocator/check] could not write the tables to "
                         "%s\n",
                         dir);
    }

    std::fflush(stderr);

    /* \~english THE VERDICT REACHES THE SHELL, which is what lets this gate a
     * build instead of being a wall of text somebody scrolls past.  `_Exit` and
     * not `exit`: we are already inside static destruction, and calling `exit`
     * from there is undefined.  The price is that the static destructors that
     * had not run yet do not run -- which is why it is a knob and not a law,
     * and why the line below says it out loud rather than leaving somebody to
     * wonder where the rest of the output went.
     *
     * \~spanish EL VEREDICTO LLEGA AL INTERPRETE DE ORDENES, que es lo que
     * permite que esto corte un build en vez de ser un muro de texto que
     * alguien pasa de largo.  `_Exit` y no `exit`: ya estamos dentro de la
     * destruccion estatica, y llamar a `exit` desde ahi es indefinido.  El
     * precio es que los destructores estaticos que faltaran no corren -- por lo
     * que es un ajuste y no una ley, y por lo que la linea de abajo lo dice en
     * voz alta en vez de dejar a alguien preguntandose donde fue el resto de la
     * salida.  \~ */
    const uint64_t proven = g_proven.load(std::memory_order_relaxed);
    const uint64_t all = g_verdicts.load(std::memory_order_relaxed);
    const uint64_t gating = g_exit_code >= 2 ? all : proven;
    if (gating != 0 && g_exit_code != 0) {
        std::fprintf(stderr,
                     "\n[allocator/check] %llu of the %llu verdicts %s: "
                     "exiting with 1.  Whatever had not been torn down yet "
                     "will not be; VESTA_ALLOC_SAN_EXITCODE=0 turns this off, "
                     "=2 makes suspicions count too.\n",
                     (unsigned long long)gating, (unsigned long long)all,
                     g_exit_code >= 2 ? "counted" : "are PROVEN");
        std::fflush(stderr);
        std::_Exit(1);
    }
}

/// \~english Runs the report last thing, and only when the checker actually
///           ran.  A destructor and not `atexit`: `atexit` allocates on some
///           runtimes, and this is the one place that must not ask the
///           allocator for anything at the end.
/// \~spanish Corre el informe lo ultimo, y solo si el comprobador llego a
///           correr.  Un destructor y no `atexit`: `atexit` reserva en algunos
///           runtimes, y este es el unico sitio que no puede pedirle nada al
///           asignador al final.
/// \~
struct Reporter {
    ~Reporter() {
        if (detail::g_san_level != SanLevel::Off) report();
    }
};
Reporter g_reporter;

} // namespace

} // namespace util

#endif // VESTA_ALLOC_SANITIZER

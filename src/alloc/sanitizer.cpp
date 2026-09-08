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
#include "util/symbols/module_symbols.h"
#include "util/symbols/self_symbols.h"

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
constexpr unsigned kFrames = 8;

/// \~english How many distinct stacks fit in the depot.  Overflow is COUNTED,
///           never quietly dropped: a depot that lies because it is full is
///           worse than a small one that says so.
/// \~spanish Cuantas pilas distintas caben en el deposito.  Pasarse se CUENTA,
///           nunca se tira en silencio: un deposito que miente por lleno es
///           peor que uno pequeno que lo dice.
/// \~
constexpr uint32_t kDepotSlots = 4096;

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
    while (n < kFrames) {
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
    for (uint32_t probe = 0; probe < kDepotSlots; ++probe) {
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
        /* Someone is filling it right now: it cannot be compared yet, and
         * waiting here would be a lock in a path that must not have one.  The
         * next slot is as good, at the price of one more entry. */
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
     * @brief When it was born, counted in ALLOCATIONS of its own thread.
     *
     * NOT A CLOCK, and that is what makes it worth having: two runs of the same
     * program give the same number, so a life can be compared between them and
     * the report can gate a build.  Wall time would answer differently every
     * run and could gate nothing.
     *
     * The counter is one the allocator ALREADY keeps -- `small_allocs` in the
     * thread's cache -- so measuring lives adds no state anywhere.  That is the
     * condition this whole mode was built under: what the checker needs, the
     * checker pays for, and only in its own build.
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

/* One row per chunk, indexed by the offset inside the chunk in units of
 * `kAlign`.  Indexing by ALIGNMENT and not by size class costs slots -- a
 * 48-byte block uses one of every three -- and buys not having to track which
 * class a chunk currently serves, which changes when a chunk is recycled.  A
 * checker that gets confused by recycling is a checker that accuses the wrong
 * line. */
constexpr uint32_t kSlotsPerChunk = kChunkBytes / kAlign;

std::atomic<Slot *> *g_rows = nullptr; ///< one entry per chunk of the region
uint32_t g_row_count = 0;

/**
 * @brief The slot of @p p, creating its row the first time.
 *
 * @return nullptr when @p p is not a small-class block of the region -- a span,
 *         the big region, a direct block, or not ours at all.  That answer is
 *         COUNTED, because it is exactly what the report has to admit it did
 *         not look at.
 */
Slot *slot_of(const void *p, size_t *block_bytes) noexcept {
    if (!in_region(p)) return nullptr;
    ChunkHeader *h = chunk_of(const_cast<void *>(p));
    if (h->magic != kChunkMagic) return nullptr; // a span, not class blocks
    /* The size of the BLOCK, which is not what the caller asked for.  Poisoning
     * and the canary both need it: writing `req + something` bytes would run
     * into the next block, and a checker that corrupts memory is worse than no
     * checker at all. */
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
 * @brief What the blocks of one site did, added up.
 *
 * THIS IS THE POINT OF THE WHOLE SHADOW, and it is worth saying plainly: the
 * two axes the allocator lets a caller DECLARE -- how long a block lives and
 * whether it grows -- come out of here MEASURED.  A declaration then stops
 * being the source of truth and becomes a claim that can be checked against the
 * data, which is what `@complexity` is to cost.
 *
 * And it matters more than it looks, because almost nothing declares anything:
 * the whole VM reports `unknown`.  Measuring does not need the hundred sites to
 * be visited one by one.
 */
struct Life {
    uint64_t deaths;   ///< blocks of this site that were released
    uint64_t life_sum; ///< total life, in allocations of the owning thread
    uint64_t unknown;  ///< released on another thread: lives not comparable
    uint32_t life_max;
    uint32_t size_min;
    uint32_t size_max;
};

Life *g_life = nullptr;

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
[[gnu::always_inline]] inline uint32_t thread_allocs(
    const detail::ThreadCache *c) noexcept {
    if (!detail::have_cache(c)) return 0;
    uint64_t n = 0;
    for (unsigned i = 0; i < VESTA_ALLOC_TAG_SLOTS; ++i) n += c->stats.by_tag[i];
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
    /* A LIFE ONLY MEANS SOMETHING WITHIN ONE THREAD: the counter is that
     * thread's, so subtracting one thread's from another's would produce a
     * number that looks like a life and is not one.  Those are counted apart
     * rather than folded in, which is the difference between "we do not know"
     * and "we know something wrong". */
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

// =========================================================================
//  Pages of its own, with a guard behind them
// =========================================================================

/**
 * @brief What is known about a block that lives on pages of its own.
 *
 * A SIDE TABLE and not a header in front of the block, and the reason is the
 * whole trick: the block is placed FLUSH against the guard page, so there is no
 * room behind it, and what is in front of it is a variable amount of slack that
 * cannot be found again from the pointer alone.
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
};

/// Open addressing, never evicting.  Full is COUNTED and the level falls back
/// for that allocation, which is loud; evicting would lose a block's identity
/// and turn a use-after-free into a wrong accusation.
constexpr uint32_t kGuardSlots = 1u << 16;
Guarded *g_guarded = nullptr;
std::atomic<uint64_t> g_guard_full{0};
std::atomic<uint64_t> g_guard_bytes{0};

uint32_t guard_hash(const void *p) noexcept {
    uint64_t v = reinterpret_cast<uintptr_t>(p) >> 4;
    v *= 0x9E3779B97F4A7C15ull;
    return uint32_t(v >> 48) & (kGuardSlots - 1);
}

/// The entry for @p p, or nullptr.  Never inserts: looking up must not create.
Guarded *guard_find(const void *p) noexcept {
    if (g_guarded == nullptr) return nullptr;
    uint32_t i = guard_hash(p);
    for (uint32_t probe = 0; probe < kGuardSlots; ++probe) {
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
    for (uint32_t probe = 0; probe < kGuardSlots; ++probe) {
        Guarded &g = g_guarded[i];
        const void *expected = nullptr;
        if (g.p.load(std::memory_order_relaxed) == nullptr &&
            g.p.compare_exchange_strong(expected, p, std::memory_order_acq_rel,
                                        std::memory_order_relaxed))
            return &g;
        i = (i + 1) & (kGuardSlots - 1);
    }
    g_guard_full.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

// =========================================================================
//  Saying it
// =========================================================================

/**
 * @brief One frame, resolved by asking WHOSE the address is FIRST.
 *
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
 */
void print_frame(const void *pc) noexcept {
    VestaModuleInfo m;
    if (vesta_module_of(pc, &m) && !vesta_module_is_self(pc)) {
        const char *path = m.path != nullptr ? m.path : "?";
        /* Just the file name: a full path buries the one part that matters in
         * a line of directories nobody reads. */
        for (const char *s = path; *s != '\0'; ++s)
            if (*s == '/' || *s == '\\') path = s + 1;
        std::fprintf(stderr, "      %s +0x%zx\n", path, m.offset);
        return;
    }
    size_t off = 0;
    const char *name = self_symbol(pc, &off);
    if (name != nullptr)
        std::fprintf(stderr, "      %s +0x%zx\n", name, off);
    else
        std::fprintf(stderr, "      %p\n", pc);
}

void print_stack(const char *what, uint32_t id) noexcept {
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

/// Whether the guard bytes fit behind @p req inside a block of @p block bytes.
/// Asked on both sides -- writing it and checking it -- so neither has to
/// remember what the other did.
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

/// A number from the environment, clamped.  Clamped and not rejected because a
/// typo in a knob must not silently switch the checker off: asking for level 9
/// gets the highest there is, which is what the person meant.
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
 * @brief Sets the checker up, and says whether it can work yet.
 *
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
 */
/**
 * @brief The knobs, the depot and the guarded table.  Everything but the
 *        shadow.
 *
 * SEPARATE FROM THE SHADOW because they become usable at different moments, and
 * tying them together made the guard level never run: the shadow needs the
 * allocator's region, which on the very first allocation of the process does
 * not exist yet -- and a guarded block does not need the shadow at all, since
 * it lives on pages of its own with its own table.  Asking for both meant the
 * first allocation was never guarded, and in a program whose first allocation
 * is the one being tested, that means NONE of them were.
 */
bool ensure_config() noexcept;

bool ensure_ready() noexcept {
    if (!ensure_config()) return false;
    if (detail::g_san_level == SanLevel::Off) return false;
    if (g_rows != nullptr) return true;

    /* Sized by what the region ACTUALLY reserved, never by `kRegionBytes`: the
     * region asks for a maximum and takes what the system gives, so the
     * constant would size this for memory that may not exist. */
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
            g_guard_edge = SanGuard(env_num("VESTA_ALLOC_SAN_GUARD",
                                            unsigned(SanGuard::Overflow),
                                            unsigned(SanGuard::Underflow)));
            g_exit_code = env_num("VESTA_ALLOC_SAN_EXITCODE", 1, 2);

            /* The table for the guarded blocks, and only when that level was
             * asked for: it is two and a half megabytes, and a level that is
             * not in force should not cost them. */
            if (detail::g_san_level >= SanLevel::Guard) {
                void *t = os_alloc(sizeof(Guarded) * kGuardSlots, kOsReadWrite);
                if (t != nullptr) {
                    std::memset(t, 0, sizeof(Guarded) * kGuardSlots);
                    g_guarded = static_cast<Guarded *>(t);
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

/* THE SET-UP GOES FIRST, IN BOTH ENTRIES, and it is not a detail: the level
 * starts at `Off` and only becomes what the environment asked for inside
 * `ensure_ready`.  Testing the level before running it means the answer is
 * always `Off`, the checker never starts, and it reports a clean run on a
 * program full of mistakes -- which is the one failure mode a checker must not
 * have.  Found by pointing it at four deliberate bugs and getting zero.
 *
 * And it has to be in `san_grow` too, not only in `san_on_alloc`: `san_grow` is
 * what reserves the room the canary is written into, so if it sits out the
 * first allocation while `san_on_alloc` does not, the canary goes past the end
 * of a block that has no room for it. */
/**
 * @brief The one entry the allocator calls, which decides everything else.
 *
 * ONE OUT-OF-LINE CALL, and the reason is measured: with three -- one to grow
 * the request, one to try the guarded path, one to record the block -- an
 * allocation cost 12.4 ns against the 4.9 of a build without the checker, with
 * the checker switched OFF at run time.  Two thirds of that was asking three
 * separate times whether there was anything to do.
 *
 * The set-up question is asked ONCE here, and when the answer is no the whole
 * thing is a call, a compare and the ordinary path.
 */
/* The three that used to be public and are now only reachable through the door
 * above.  Declared here because that door is defined first, and it reads better
 * first: it is the one thing the allocator knows about. */
size_t san_grow(size_t n) noexcept;
void *san_alloc_guarded(size_t n, const void *pc, const void *fp) noexcept;
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

    void *p = san_alloc_guarded(n, pc, fp);
    if (p == nullptr) p = detail::alloc_body(san_grow(n));
    san_on_alloc(p, n, pc, fp);
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
void *san_alloc_guarded(size_t n, const void *pc, const void *fp) noexcept {
    /* The CONFIG and not the whole set-up: a guarded block does not touch the
     * shadow, so waiting for the shadow would keep the very first allocation of
     * the process -- and in a small program, every allocation -- out of the
     * level that was asked for. */
    if (!ensure_config() || detail::g_san_level < SanLevel::Guard)
        return nullptr;
    if (g_guarded == nullptr || n == 0) return nullptr;

    const size_t page = os_page_size();
    const size_t data = (n + kCanaryBytes + page - 1) / page * page;
    const size_t total = data + page; // the guard
    if (data < n) return nullptr;     // wrapped: refuse rather than serve wrong

    /* Reserved WITHOUT permissions and then only the data pages committed: what
     * is left is the guard, and it is unmapped because nobody ever asked for
     * it, not because something took it away. */
    void *base = os_reserve(total);
    if (base == nullptr) return nullptr;
    if (!os_commit(base, data, kOsReadWrite)) {
        os_free(base, total);
        return nullptr;
    }

    unsigned char *const start = static_cast<unsigned char *>(base);
    unsigned char *p;
    size_t tail;
    if (g_guard_edge == SanGuard::Underflow) {
        /* The other edge: the block starts where the mapped pages start, so
         * reading or writing BEFORE it is what faults.  Then the guard is the
         * page before, and nothing watches the end. */
        p = start + page;
        tail = 0;
    } else {
        const uintptr_t end = reinterpret_cast<uintptr_t>(start) + data;
        const uintptr_t want = (end - n) & ~uintptr_t(kAlign - 1);
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
    g->alloc_stack = intern_stack(frames, nf, walked);
    g->free_stack = 0;
    g->state = kStAlive;
    g_guard_bytes.fetch_add(total, std::memory_order_relaxed);

    /* The gap between the end of the block and the guard, filled so that an
     * overflow too small to reach the page still leaves a mark. */
    if (tail != 0) std::memset(p + n, kCanaryByte, tail);
    return p;
}

/// @return true when the release was handled here and must not go any further.
bool guarded_free(void *p, const void *fp) noexcept {
    Guarded *g = guard_find(p);
    if (g == nullptr) return false;

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

    /* DECOMMITTED, NOT FREED.  The pages go, so touching the block from now on
     * faults where it is touched; the range stays ours, so the address is never
     * handed to anybody else and the fault is always about THIS block.  That is
     * what makes a use-after-free point at the right code -- and what makes
     * this level expensive. */
    os_decommit(const_cast<void *>(g->base), g->total - os_page_size());
    return true;
}

void san_on_alloc(void *p, size_t req, const void *pc,
                  const void *fp) noexcept {
    if (p == nullptr) return;

    /* A guarded block wrote its own entry when it was served, and it does not
     * live in the region, so the shadow would count it as something it could
     * not look at -- which would be a lie in the other direction. */
    if (detail::g_san_level >= SanLevel::Guard && guard_find(p) != nullptr)
        return;

    if (!ensure_ready()) return;
    size_t block = 0;
    Slot *s = slot_of(p, &block);
    if (s == nullptr) {
        g_uncovered.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    /* A WRITE AFTER FREE, caught where it can be caught without a page each:
     * the block was poisoned when it was released, so anything that is not the
     * poison now was written by somebody who no longer owned it. */
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
    s->free_stack = 0;
    s->req = uint32_t(req);
    s->meta = meta_of(kStAlive, tid, 0);
    /* Its birthday, in allocations of this thread.  Read from counters the
     * allocator already keeps, so nothing new is stored anywhere. */
    s->seq = thread_allocs(c);

    /* ONLY IF IT FITS, and the condition is checked and not assumed.  On the
     * very first allocation of the process `san_grow` sits out -- the region it
     * needs does not exist yet -- while this call, one instant later, finds it
     * ready.  Writing the canary then would put it past the end of a block that
     * was never grown to hold it: the checker corrupting memory.
     *
     * The same condition is asked again when the block is released, so the two
     * sides agree without having to remember anything. */
    if (detail::g_san_level >= SanLevel::Canary && canary_fits(req, block))
        write_canary(p, req);
}

[[gnu::noinline]] bool san_on_free(void *p) noexcept {
    if (p == nullptr) return true;
    if (!ensure_config() || detail::g_san_level == SanLevel::Off) return true;

    /* A guarded block never goes back to the allocator: it was never served by
     * it, and handing it over would send a pointer from outside the region to
     * `no_foreign_free`, which stops the process.  Answering false is what
     * keeps it here -- and it is asked BEFORE the shadow, which that block does
     * not have. */
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
        /* A DOUBLE FREE, and it is PROVEN: this exact block is already on a
         * free list.  Letting it through would push it a second time and hand
         * one block to two owners, so the checker would have turned a reported
         * bug into a corrupted heap.  That is why this answers false. */
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
        /* Ours by address, but we never saw it handed out.  That is what a
         * block allocated before the checker was ready looks like, and also
         * what a block from a door that is not instrumented looks like, so it
         * is SUSPECTED and the release goes ahead: refusing would break a
         * program that is doing nothing wrong. */
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

    /* Allocated on one thread and released on another.  NOT a verdict: it is
     * legal and the allocator handles it.  It is counted because it is the
     * shape of ownership crossing threads by accident, which is worth seeing
     * even when nothing is broken. */
    const bool same_thread = meta_alloc_thread(s->meta) == tid;
    if (!same_thread) g_cross_thread.fetch_add(1, std::memory_order_relaxed);

    /* WHAT THIS BLOCK TURNED OUT TO BE.  Here, where it dies, is the only place
     * that knows how long it lived -- and adding it up per site is what turns
     * the two axes from something a caller declares into something the run
     * MEASURES. */
    note_death(s->alloc_stack, s->req, same_thread, s->seq, thread_allocs(c));

    s->free_stack = intern_stack(frames, n, walked);
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

namespace {

/// One line of the leak report: a stack and what is still hanging off it.
struct Leak {
    uint32_t stack;
    uint32_t blocks;
    uint64_t bytes;
};

/// Sorted by bytes and then by stack id, which is what makes two runs of the
/// same program print the same list.  A report that reorders itself between
/// runs cannot gate a build, and gating a build is the point.
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
 * @brief Walks the shadow at exit and says what never came back.
 *
 * WHY THE MODULE IS ASKED HERE AND NOT WHEN ALLOCATING: because the allocator
 * must not pay for the checker's comfort.  The shadow keeps a raw address; who
 * it belongs to is worked out ONCE, at the end, and only for the blocks that
 * survived.  It is also what replaces a suppression list -- third-party
 * libraries leak on purpose, and a list of exceptions is how a checker starts
 * lying, so the split comes out DERIVED from where the code lives.
 */
void report() noexcept {
    if (g_rows == nullptr || g_depot == nullptr) return;

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

    /* WHAT IT DID NOT LOOK AT, and it goes FIRST.  A checker that stays quiet
     * about its blind spots is read as "there is nothing there", which is the
     * one way this whole thing could do harm. */
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
    if (gb != 0)
        std::fprintf(stderr,
                     "[allocator/check] %llu MiB of address space went to "
                     "guarded blocks and is NOT coming back: that is what buys "
                     "the fault happening where the mistake is\n",
                     (unsigned long long)(gb / (1024 * 1024)));

    const uint64_t cross = g_cross_thread.load(std::memory_order_relaxed);
    if (cross != 0)
        std::fprintf(stderr,
                     "[allocator/check] %llu blocks were allocated on one "
                     "thread and released on another -- legal, and worth "
                     "seeing\n",
                     (unsigned long long)cross);

    /* WHAT THE SITES ARE, measured.  This is the half of the report that is not
     * about mistakes: it says, for every place that allocates, how long its
     * blocks lived and whether they were all the same size -- which is exactly
     * the two axes a caller can declare.  Declaring then stops being the source
     * of truth and becomes a claim with something to check it against. */
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

    if (alive == 0) {
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
    std::fflush(stderr);

    /* THE VERDICT REACHES THE SHELL, which is what lets this gate a build
     * instead of being a wall of text somebody scrolls past.
     *
     * `_Exit` and not `exit`: we are already inside static destruction, and
     * calling `exit` from there is undefined.  The price is that the static
     * destructors that had not run yet do not run -- which is why it is a knob
     * and not a law, and why the line below says it out loud rather than
     * leaving somebody to wonder where the rest of the output went. */
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

/// Runs the report last thing, and only when the checker actually ran.  A
/// destructor and not `atexit`: `atexit` allocates on some runtimes, and this
/// is the one place that must not ask the allocator for anything at the end.
struct Reporter {
    ~Reporter() {
        if (detail::g_san_level != SanLevel::Off) report();
    }
};
Reporter g_reporter;

} // namespace

} // namespace util

#endif // VESTA_ALLOC_SANITIZER

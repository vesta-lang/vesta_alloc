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

/// How deep a stack is walked.  Eight is what makes a leak report name the
/// culprit instead of the helper it went through, and it is still one cache
/// line of pointers.
constexpr unsigned kFrames = 8;

/// How many distinct stacks fit in the depot.  Overflow is COUNTED, never
/// quietly dropped: a depot that lies because it is full is worse than a small
/// one that says so.
constexpr uint32_t kDepotSlots = 4096;

/// The byte a released block is filled with.  A pointer built out of it is
/// wildly unmapped on both systems, and read as a number it is recognisable at
/// a glance in a debugger.
constexpr unsigned char kPoisonByte = 0xDD;

/// The pattern written behind the caller's bytes, and how much of it.  Four
/// bytes, so a single off-by-one is caught and no plausible memcpy reproduces
/// it by accident.
constexpr unsigned char kCanaryByte = 0xAB;
constexpr size_t kCanaryBytes = 4;

/// How much of a released block is poisoned; see @c SanPoison.
SanPoison g_poison = SanPoison::Line;

/// Which edge @c SanLevel::Guard puts the block against; see @c SanGuard.
SanGuard g_guard_edge = SanGuard::Overflow;

/**
 * @brief What makes the process exit non-zero.  0 = nothing, 1 = proven,
 *        2 = proven and suspected.
 *
 * ONE IS THE DEFAULT, AND WHICH ONE IT IS MATTERS MORE THAN IT LOOKS.  A block
 * still alive at exit is SUSPECTED, never proven: memory a program keeps on
 * purpose until it dies looks exactly the same.  Failing a build on that was
 * the first thing this mode did, and it took ten of the library's own tests
 * down with it -- every one of them for keeping something alive on purpose.
 *
 * A checker that cries wolf gets switched off and never comes back, so a
 * suspicion is REPORTED and does not gate.  Two is there for a codebase that
 * has decided it wants zero blocks alive at exit and is willing to chase them.
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
 * @brief How sure the checker is, and it travels WITH the verdict.
 *
 * Not decoration.  "Could not prove this is right" is not "proved it is wrong"
 * -- the lesson `may_alias` already taught this codebase the hard way -- and a
 * list that mixes the two gets ignored whole.  A trampled canary is PROVEN; a
 * block still alive at exit may be a leak or may be memory the program keeps on
 * purpose, so it is SUSPECTED.
 */
enum class Certainty { Proven, Suspected };

const char *certainty_word(Certainty c) noexcept {
    return c == Certainty::Proven ? "PROVEN" : "SUSPECTED";
}

// =========================================================================
//  The stack depot
// =========================================================================

/// A slot is empty, being filled, or readable.  The middle state exists because
/// a reader must never compare against half-written frames and conclude it has
/// found a different stack.
enum : uint32_t { kSlotEmpty = 0, kSlotClaiming = 1, kSlotReady = 2 };

struct Stack {
    std::atomic<uint32_t> state;
    uint8_t frames; ///< how many entries of `pc` are real
    uint8_t walked; ///< 1 = frame chain followed, 0 = one return address only
    const void *pc[kFrames];
};

Stack *g_depot = nullptr;

/**
 * @brief Whether @p fp can be a frame pointer of the stack we are standing on.
 *
 * WITHOUT `-fno-omit-frame-pointer` whatever sits in that register is just a
 * number, and following it INVENTS a stack.  An invented stack is worse than
 * one true frame, because it names functions that had nothing to do with it.
 * So every step is validated and the walk stops at the first one that is not:
 * the verdict then says it carries a single frame, which is the truth.
 *
 * Cheap and enough: it has to point up the stack from where we are, be aligned,
 * and not be further away than a stack could plausibly be.
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
 * @brief Fills @p out with the caller's stack, as deep as it can PROVE.
 *
 * @param out    where the frames go; at least @c kFrames of them.
 * @param first  the return address the caller already holds.  It is always
 *               frame zero, and it is the WHOLE answer when there is no frame
 *               chain to follow -- which is the old one-address model, kept as
 *               a fallback and not as a degraded case.
 * @param walked set to true only if at least one frame came from the chain.
 * @return how many frames are real.
 */
unsigned walk_stack(const void **out, const void *first,
                    bool *walked) noexcept {
    out[0] = first;
    *walked = false;
    unsigned n = 1;

    /* From this function's own frame the chain is [saved base pointer][return
     * address], which is what both compilers emit whenever they keep the frame
     * pointer at all.  When they do not, `plausible_frame` throws it out on the
     * first step and we keep the one true address. */
    const void *fp = __builtin_frame_address(0);
    const void *below = fp;
    while (n < kFrames) {
        const void *const *slot = static_cast<const void *const *>(fp);
        const void *const next = slot[0];
        const void *const ret = slot[1];
        if (ret == nullptr || !plausible_frame(next, below)) break;
        /* THE FIRST ONE IS USUALLY THE ONE WE ALREADY HAVE.  Whether the hook
         * ends up as its own frame depends on what the compiler inlined into
         * what, so the chain may start at the very address the caller handed
         * us.  Skipping it by position would be right in one build and wrong in
         * the next; comparing is right in both. */
        if (ret != out[0]) {
            out[n++] = ret;
            *walked = true;
        }
        below = fp;
        fp = next;
    }
    return n;
}

/// FNV-1a over the frames.  The same stack lands on the same slot every run,
/// which is what makes the report reproducible -- and a report that changes
/// between runs cannot gate a build.
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
 * @brief Stores a stack and hands back its id, reusing the one already there.
 *
 * Open addressing, APPEND ONLY: a full depot never evicts.  Evicting would make
 * two different stacks share an id and the report would name the wrong
 * function, which is the one failure a checker cannot have.  Full is counted
 * (@c g_depot_full) and answered with zero, which the report prints as "stack
 * not available" instead of as somebody's stack.
 *
 * @return the id, or 0 when there is no stack to give.
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
};
static_assert(sizeof(Slot) == 16, "the shadow costs 16 bytes per block");

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
 * @brief The part of a released block the poison may touch.
 *
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
 * @param block the size of the block, which is what bounds this: one byte past
 *              it would be the checker corrupting the NEXT block.
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
bool ensure_ready() noexcept {
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

            /* THE LEVEL THAT IS NOT BUILT YET SAYS SO.  Answering to
             * `SanLevel::Guard` by quietly doing what `Poison` does would be a
             * checker claiming to watch every write when it watches none of
             * them until the block comes back -- the exact silence this whole
             * mode exists to avoid.  So it is announced, and what it actually
             * does is stated. */
            if (detail::g_san_level == SanLevel::Guard) {
                std::fprintf(stderr,
                             "[allocator/check] level %u (a page per block) is "
                             "NOT BUILT YET: running at level %u instead, which "
                             "catches an overflow when the block is released "
                             "and not at the instant of the write\n",
                             unsigned(SanLevel::Guard),
                             unsigned(SanLevel::Poison));
                detail::g_san_level = SanLevel::Poison;
            }
            void *depot = os_alloc(sizeof(Stack) * kDepotSlots, kOsReadWrite);
            if (depot != nullptr) {
                std::memset(depot, 0, sizeof(Stack) * kDepotSlots);
                g_depot = static_cast<Stack *>(depot);
            }
            cfg.store(2, std::memory_order_release);
        } else {
            return false; // somebody else is in there; sit this one out
        }
    }
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
size_t san_grow(size_t n) noexcept {
    if (!ensure_ready()) return n;
    if (detail::g_san_level < SanLevel::Canary) return n;
    if (n > size_t(-1) - kCanaryBytes) return n; // no wrapping, ever
    return n + kCanaryBytes;
}

[[gnu::noinline]] void san_on_alloc(void *p, size_t req) noexcept {
    if (p == nullptr) return;
    if (!ensure_ready() || detail::g_san_level == SanLevel::Off) return;

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
    const unsigned n =
        walk_stack(frames, __builtin_return_address(0), &walked);

    const detail::ThreadCache *c = detail::current_cache();
    const uint32_t tid = detail::have_cache(c) ? c->id : 0;

    s->alloc_stack = intern_stack(frames, n, walked);
    s->free_stack = 0;
    s->req = uint32_t(req);
    s->meta = meta_of(kStAlive, tid, 0);

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
    if (!ensure_ready() || detail::g_san_level == SanLevel::Off) return true;

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
            walk_stack(frames, __builtin_return_address(0), &walked);
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
        walk_stack(frames, __builtin_return_address(0), &walked);
    const detail::ThreadCache *c = detail::current_cache();
    const uint32_t tid = detail::have_cache(c) ? c->id : 0;

    /* Allocated on one thread and released on another.  NOT a verdict: it is
     * legal and the allocator handles it.  It is counted because it is the
     * shape of ownership crossing threads by accident, which is worth seeing
     * even when nothing is broken. */
    if (meta_alloc_thread(s->meta) != tid)
        g_cross_thread.fetch_add(1, std::memory_order_relaxed);

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

    const uint64_t cross = g_cross_thread.load(std::memory_order_relaxed);
    if (cross != 0)
        std::fprintf(stderr,
                     "[allocator/check] %llu blocks were allocated on one "
                     "thread and released on another -- legal, and worth "
                     "seeing\n",
                     (unsigned long long)cross);

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

/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file src/malloc_define.cpp
 * @brief BEING `malloc`, instead of being renamed into it.
 *
 * WHAT THIS BUYS OVER `--wrap`, and it is one thing only, but it is the thing
 * the other way cannot do.  Link-time renaming reaches every call in every
 * object of the link -- and stops there.  What the C library allocates INSIDE
 * itself never becomes a pending reference, so no linker can rename it: that is
 * `strdup`, `getline`, `asprintf`, `realpath` and the rest of the family that
 * hands the caller a block to release.  Today that memory does not appear in
 * the report at all, which reads like "there is none".
 *
 * Defining the symbol closes it.  On ELF a definition in the executable wins
 * over the one in `libc.so`, and the library's own calls go through the PLT --
 * so they land here too.  It is what jemalloc, tcmalloc and mimalloc do, and it
 * is the only mechanism that reaches inside a library nobody recompiled.
 *
 * ELF ONLY, and not for lack of trying: on Windows the C runtime is a DLL and a
 * caller reaches it through the import table, so there is no symbol to outrank.
 * That side is served by `malloc_interpose.cpp` and its `__imp__` pointers.
 *
 * ------------------------------------------------------------------------
 * WHY ALL FOUR, AND NOT JUST `malloc` AND `free`
 * ------------------------------------------------------------------------
 *
 * Because the moment `malloc` is ours, the C library holds OUR blocks -- and
 * then it grows them with ITS `realloc`, which reads a header we never wrote.
 * That is not a scenario, it is what the compiler did on its second line of
 * work, and the backtrace named the caller exactly:
 *
 *     musable (mem=0x7fbbf7900010)             <- reads glibc's chunk header
 *     __GI___libc_realloc (oldmem=..., bytes=8)
 *     __GI___getcwd (buf=0x0)                  <- asks malloc, then SHRINKS
 *     std::filesystem::current_path()
 *
 * `getcwd(NULL, 0)` allocates the buffer it returns and then trims it to the
 * length it actually used.  So does `vasprintf`, and so does `getline` when it
 * grows.  Leaving `realloc` to the C library is therefore not a smaller version
 * of this mechanism -- it is a mechanism that CORRUPTS, and it corrupts on
 * blocks the other half handed out on purpose.
 *
 * `calloc` is here for the other half of the same rule.  It cannot corrupt --
 * nothing hands it an existing pointer -- but a block the library zeroed for
 * itself would be the one kind of allocation that never reaches the report, and
 * a report with a hole in it is worse than no report: it reads as an answer.
 *
 * ------------------------------------------------------------------------
 * WHY IT CANNOT SHARE THE ROAD WITH `--wrap`
 * ------------------------------------------------------------------------
 *
 * With `--wrap=malloc` in force AND a definition of `malloc` present, the
 * linker resolves `__real_malloc` against the only definition of `malloc` in
 * the link, which is this one.  The first allocation then calls itself until
 * the stack is gone.  Measured with a three-line probe before any of this was
 * written, because it is the kind of thing that has to be known and not
 * guessed.  So the build removes `--wrap=malloc` and `--wrap=free` when this
 * file is in; see the option in `CMakeLists.txt`.
 *
 * ------------------------------------------------------------------------
 * THE HARD PART: BEING `malloc` BEFORE THE ALLOCATOR EXISTS
 * ------------------------------------------------------------------------
 *
 * The allocator decides whether it is active on its FIRST allocation, and while
 * it is deciding it answers "not active" on purpose -- so that a request made
 * from inside that decision does not re-enter it and hang.  With `--wrap` that
 * gap is harmless: by the time anything in the program calls `malloc`, the C
 * runtime has long since started.  Being `malloc` moves the gap to the worst
 * possible place: the process start-up walks straight through it, and so does
 * the `dlsym` below.
 *
 * And what lives at the bottom of that gap is not a fallback -- it is
 * `no_fallback`, which prints and KILLS the process.  That is the right answer
 * when nobody can allocate except us; it is the wrong answer for the twenty
 * bytes the loader asks for before we are ready.
 *
 * Hence the bootstrap arena: a fixed buffer that serves that window and nothing
 * else.  It never recycles -- start-up allocations live as long as the process
 * anyway -- so it is a bump pointer, and recognising one of its blocks is the
 * same two comparisons that recognise one of the allocator's.  It sits in
 * `.bss`, so it costs address space and not resident memory: a page appears
 * only when it is touched.
 */

/* `RTLD_NEXT` is a GNU extension and has to be asked for before the first
 * header, or `<dlfcn.h>` hides it and the only symptom is a name that does not
 * exist. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "interpose_common.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>

namespace {

using vesta_interpose::note_site;
using vesta_interpose::ours;

/**
 * @brief How much the bootstrap arena can serve, in total, ever.
 *
 * Sized for what the C library and the dynamic loader ask for before the
 * allocator has made up its mind, which is a handful of small blocks.  64 KiB
 * is far more than that has ever been measured to need, and it is in `.bss`:
 * the pages that are never touched are never real.  Running out is a loud
 * failure and not a silent one -- see @c boot_alloc.
 */
constexpr size_t kBootBytes = 64u * 1024u;

alignas(util::kAlign) unsigned char g_boot[kBootBytes];
std::atomic<size_t> g_boot_used{0};

/**
 * @brief Each bootstrap block carries its size in front of it.
 *
 * A bump arena does not need this to hand memory out -- but `realloc` does, and
 * one of these blocks can perfectly well reach it: `getline` grows its buffer,
 * and `getline` runs during start-up.  Without the size there is no way to know
 * how much to copy, and guessing means either losing data or reading past the
 * block.  A whole alignment unit so what follows stays aligned to 16.
 */
constexpr size_t kBootHeader = util::kAlign;
static_assert(kBootHeader >= sizeof(size_t),
              "the bootstrap header has to hold a size");

/**
 * @brief Serves out of the bootstrap arena.  Bump pointer, no freeing.
 *
 * Thread safe, and it has to be: nothing says the start-up window belongs to
 * one thread, and a library constructor is free to start one.  The loop is the
 * ordinary compare-and-swap bump -- there is no other state to protect.
 */
void *boot_alloc(size_t n) noexcept {
    if (n == 0) n = 1; // a unique pointer, as the standard asks
    const size_t body = (n + util::kAlign - 1) & ~(size_t)(util::kAlign - 1);
    /* Checked before adding the header, because a size that came from a caller
     * can be large enough to wrap the sum -- and a wrapped sum passes every
     * test that follows it. */
    if (body > kBootBytes || n > (size_t)-1 - util::kAlign) return nullptr;
    const size_t want = body + kBootHeader;

    size_t old = g_boot_used.load(std::memory_order_relaxed);
    size_t next = 0;
    do {
        /* The subtraction and not `old + want > kBootBytes`, for the same
         * reason: this way nothing can wrap. */
        if (want > kBootBytes - old) {
            std::fprintf(stderr,
                         "[allocator] PANIC: the bootstrap arena is full "
                         "(%zu bytes) and %zu more were asked for before the "
                         "allocator was ready. Raise kBootBytes in "
                         "malloc_define.cpp.\n",
                         kBootBytes, want);
            return nullptr;
        }
        next = old + want;
    } while (!g_boot_used.compare_exchange_weak(old, next,
                                                std::memory_order_acq_rel,
                                                std::memory_order_relaxed));
    unsigned char *block = g_boot + old;
    *reinterpret_cast<size_t *>(block) = n;
    return block + kBootHeader;
}

/**
 * @brief The C library's own `free`, for blocks that are not ours.
 *
 * WHY THIS IS NEEDED AT ALL.  The dynamic loader allocates before the program
 * has symbols of its own, and some of what it produces is handed back for the
 * caller to release.  With `--wrap` that block goes to `__real_free`; here
 * there is no `__real_` anything, so the real one has to be looked up.
 *
 * Resolved ONCE and remembered, and resolved LAZILY rather than in a
 * constructor: `dlsym` allocates, and a constructor would run it at a moment we
 * do not control.  Asked for on the first foreign block instead, when the
 * bootstrap arena is already able to answer whatever `dlsym` needs.
 *
 * `RTLD_NEXT` and not `RTLD_DEFAULT`: the default would find OUR definition --
 * we are the first `free` in the search order -- and call it again.
 */
using FreeFn = void (*)(void *);
std::atomic<FreeFn> g_real_free{nullptr};

FreeFn real_free() noexcept {
    FreeFn f = g_real_free.load(std::memory_order_acquire);
    if (f != nullptr) return f;
    f = reinterpret_cast<FreeFn>(::dlsym(RTLD_NEXT, "free"));
    g_real_free.store(f, std::memory_order_release);
    return f;
}

/**
 * @brief The C library's own `realloc`, for the same reason and found the same
 *        way.
 *
 * Only ever reached for a block that is neither ours nor the bootstrap arena's
 * -- which means the dynamic loader made it before we were anybody -- and that
 * block can only be resized by whoever knows how big it is.
 */
using ReallocFn = void *(*)(void *, size_t);
std::atomic<ReallocFn> g_real_realloc{nullptr};

ReallocFn real_realloc() noexcept {
    ReallocFn f = g_real_realloc.load(std::memory_order_acquire);
    if (f != nullptr) return f;
    f = reinterpret_cast<ReallocFn>(::dlsym(RTLD_NEXT, "realloc"));
    g_real_realloc.store(f, std::memory_order_release);
    return f;
}

/**
 * @brief
 * \~english The C library's own `malloc_usable_size`, found the same way.
 * \~spanish El `malloc_usable_size` de la propia libreria de C, encontrado
 *           igual.
 * \~
 *
 * \~english
 * Only ever reached for a block the loader made before we were anybody.  Unlike
 * the Windows side, where the runtime's entry has been overwritten and there is
 * nothing left to call, here the real one is still there and still knows the
 * answer -- so a foreign block gets a TRUE size instead of a defensible refusal.
 *
 * \~spanish
 * Solo se llega para un bloque que hizo el cargador antes de que fueramos
 * nadie.  A diferencia del lado de Windows, donde la entrada del runtime esta
 * pisada y no queda nada a lo que llamar, aqui la de verdad sigue ahi y sigue
 * sabiendo la respuesta -- asi que un bloque ajeno recibe un tamano CIERTO en
 * vez de una negativa defendible.
 * \~
 */
using UsableFn = size_t (*)(void *);
std::atomic<UsableFn> g_real_usable{nullptr};

UsableFn real_usable_size() noexcept {
    UsableFn f = g_real_usable.load(std::memory_order_acquire);
    if (f != nullptr) return f;
    f = reinterpret_cast<UsableFn>(::dlsym(RTLD_NEXT, "malloc_usable_size"));
    g_real_usable.store(f, std::memory_order_release);
    return f;
}

/**
 * @brief A pointer arriving at `realloc` that nobody can account for.
 *
 * The sibling of `no_foreign_free`, and separate from it so the message names
 * what actually happened: sending someone to look at their `free` calls when
 * the block came through `realloc` is a diagnostic that costs an afternoon.
 *
 * Returning null instead would be legal -- that is what `realloc` says when it
 * cannot grow -- and it would be the wrong answer here: the caller would carry
 * on with a block nobody owns, and the failure would surface somewhere else
 * entirely.
 */
[[noreturn, gnu::noinline, gnu::cold]] void no_foreign_realloc(void *p) noexcept {
    std::fprintf(stderr,
                 "[allocator] PANIC: realloc() of %p, which this allocator "
                 "never handed out, and the C library's own realloc could not "
                 "be found to hand it back to.\n"
                 "            A pointer arriving here came through a door we "
                 "are not watching.\n",
                 p);
    std::fflush(stderr);
    std::abort();
}

/**
 * @brief The allocation that `malloc` makes, and that `realloc` makes when it
 *        has to move a block out of the bootstrap arena.
 *
 * The order of the two tests is the whole design, and it is written once here
 * because both entries need it identical.  Asking the allocator whether it is
 * active is what triggers its decision, and during that decision it answers no
 * -- which routes the request to the arena instead of into the re-entry that
 * would hang.  Once it says yes it never goes back, so this costs one
 * predictable branch for the rest of the process.
 *
 * @param ret the caller's return address, taken by whoever was actually called:
 *            reading it here would name this helper's caller and not the
 *            program's.
 */
[[gnu::always_inline]] inline void *fresh_block(size_t n,
                                                const void *ret) noexcept {
    if (__builtin_expect(!util::host_alloc_active(), 0)) return boot_alloc(n);
    void *p = util::host_alloc(n);
    note_site(ret, n);
    return p;
}

} // namespace

namespace vesta_interpose {

/* The two the OTHER file needs.  Declared in `interpose_common.h`, defined
 * here, and only compiled at all when this file is: with the option off they
 * are an inline `false` and an inline zero, so nothing in the ordinary build
 * carries a branch for an arena that does not exist. */

bool from_bootstrap(const void *p) noexcept {
    const auto v = reinterpret_cast<uintptr_t>(p);
    return v >= reinterpret_cast<uintptr_t>(g_boot) &&
           v < reinterpret_cast<uintptr_t>(g_boot) + kBootBytes;
}

size_t bootstrap_size(const void *p) noexcept {
    return *reinterpret_cast<const size_t *>(
        reinterpret_cast<const unsigned char *>(p) - kBootHeader);
}

} // namespace vesta_interpose

extern "C" {

/**
 * @brief `malloc`, and this time it IS the symbol.
 *
 * See @c fresh_block for why the order of its two tests is the whole design.
 */
void *malloc(size_t n) {
    return fresh_block(n, __builtin_return_address(0));
}

/**
 * @brief `calloc`, which is `malloc` plus a promise about the contents.
 *
 * The bootstrap arena keeps that promise for free, and that is a fact about how
 * it is built and not a hope: it lives in `.bss`, so it starts zeroed, and it
 * NEVER recycles -- every byte it hands out is a byte nobody has written.  The
 * size header sits before the block, so it does not touch what the caller sees.
 * Clearing it again would be writing over zeros.
 *
 * With the allocator up, `host_alloc_zeroed` says the same thing for the same
 * reason: memory that has just come from the system is already zero, and only
 * memory that has been used before has to be cleared.
 */
void *calloc(size_t count, size_t size) {
    /* The overflow of the product is the classic `calloc` bug, and an expensive
     * one: it allocates less than asked and the caller writes more.  Checked
     * before multiplying. */
    if (count != 0 && size > (size_t)-1 / count) return nullptr;
    const size_t n = count * size;
    if (__builtin_expect(!util::host_alloc_active(), 0)) return boot_alloc(n);
    void *p = util::host_alloc_zeroed(n);
    note_site(__builtin_return_address(0), n);
    return p;
}

/**
 * @brief `realloc`, with the same three kinds of block `free` tells apart.
 *
 * THIS IS THE ONE THAT HAD TO BE HERE.  With `malloc` ours and `realloc` the C
 * library's, `getcwd(NULL, 0)` allocates through us and trims through them, on
 * a block whose header they never wrote.  See the file header for the
 * backtrace.
 *
 * Ours is the common case and goes first.  A bootstrap block cannot grow where
 * it is -- that arena only moves forward -- so it is copied out and left
 * behind, which is what the arena is for.  Anything else came from the loader
 * and goes back to the C library that made it, whole.
 */
void *realloc(void *p, size_t n) {
    /* `realloc(NULL, n)` IS `malloc(n)`, and the C library's own callers use it
     * that way to avoid writing the first-time case twice. */
    if (p == nullptr) return fresh_block(n, __builtin_return_address(0));

    if (__builtin_expect(ours(p), 1)) {
        void *q = util::host_realloc(p, n);
        /* Recorded with the NEW size, which is what was just asked for.  A
         * `realloc` is how things grow in C -- it is the `std::vector` of this
         * side -- so leaving it out would hide the very pattern worth looking
         * at. */
        note_site(__builtin_return_address(0), n);
        return q;
    }

    if (vesta_interpose::from_bootstrap(p)) {
        /* A size of zero releases the block, and releasing a bootstrap block is
         * dropping it: the arena does not recycle, by design. */
        if (n == 0) return nullptr;
        const size_t old = vesta_interpose::bootstrap_size(p);
        void *fresh = fresh_block(n, __builtin_return_address(0));
        if (fresh == nullptr) return nullptr; // `p` is still valid, as required
        vesta_memcpy(fresh, p, old < n ? old : n);
        return fresh;
    }

    const ReallocFn f = real_realloc();
    if (f != nullptr) return f(p, n);
    no_foreign_realloc(p); // does not return
}

/**
 * @brief `free`, with three kinds of block to tell apart.
 *
 * Ours is the common case and goes first.  A bootstrap block is DROPPED and
 * that is not a leak being tolerated: the arena does not recycle by design, and
 * what it served is start-up state that lives as long as the process.  Anything
 * else came from the loader, and goes back to the C library that made it.
 *
 * If even that cannot be found, the allocator's own rule applies: a pointer
 * nobody can account for is not an error to swallow.  `no_foreign_free` says so
 * and stops, rather than letting the counts quietly stop adding up.
 */
void free(void *p) {
    if (p == nullptr) return;
    if (__builtin_expect(ours(p), 1)) {
        util::host_free(p);
        return;
    }
    if (vesta_interpose::from_bootstrap(p)) return;
    const FreeFn f = real_free();
    if (f != nullptr) {
        f(p);
        return;
    }
    util::detail::no_foreign_free(p); // does not return
}

/**
 * @brief
 * \~english `malloc_usable_size`, which is the question rather than the deed.
 * \~spanish `malloc_usable_size`, que es la pregunta y no el hecho.
 * \~
 *
 * \~english
 * WHY A FUNCTION THAT ALLOCATES NOTHING IS IN AN ALLOCATOR'S LIST.  Because it
 * READS the block, and the header it reads is the one the allocator that made
 * the block wrote.  Ours have no glibc header, so leaving this to the C library
 * means it reads the eight bytes in front of one of our blocks -- which are not
 * a size, they are whatever happens to be there -- and answers with them.  It
 * does not fault: it returns a number, and a wrong size that looks right is the
 * worst answer this project admits.  The caller then writes that many bytes.
 *
 * It is the same mistake as leaving `realloc` to the C library while serving
 * `malloc`, which is what killed the compiler on Linux, and the same one that
 * `_msize` was making on Windows -- there it does fault, because the NT heap
 * checks and stops the process.  Being `malloc` by halves corrupts, and the
 * halves are not only the verbs.
 *
 * WHY IT IS NOT GUARDED BY A CONFIGURATION SWITCH.  Defining this symbol only
 * has an effect where the symbol exists, and this whole file only compiles for
 * the ELF path, where it does.
 *
 * \~spanish
 * POR QUE UNA FUNCION QUE NO RESERVA NADA ESTA EN LA LISTA DE UN ASIGNADOR.
 * Porque LEE el bloque, y la cabecera que lee es la que escribio el asignador
 * que hizo ese bloque.  Los nuestros no tienen cabecera de glibc, asi que
 * dejarle esto a la libreria de C significa que lee los ocho bytes de delante
 * de uno de nuestros bloques -- que no son un tamano, son lo que hubiera ahi --
 * y contesta con ellos.  No falla: devuelve un numero, y un tamano equivocado
 * con pinta de correcto es la peor respuesta que este proyecto admite.  Quien
 * llama escribe entonces esos bytes.
 *
 * Es la misma equivocacion que dejarle `realloc` a la libreria de C sirviendo
 * `malloc`, que es lo que mato al compilador en Linux, y la misma que hacia
 * `_msize` en Windows -- alli si falla, porque el monton NT comprueba y para el
 * proceso.  Ser `malloc` a medias corrompe, y las mitades no son solo los
 * verbos.
 *
 * POR QUE NO VA DETRAS DE UN INTERRUPTOR DE CONFIGURACION.  Definir este
 * simbolo solo tiene efecto donde el simbolo existe, y este fichero entero solo
 * se compila para el camino de ELF, donde existe.
 * \~
 *
 * @param p
 * \~english a block from any of the three sources, or null.
 * \~spanish un bloque de cualquiera de las tres procedencias, o nulo.
 * \~
 * @return
 * \~english how much may be written, which may be more than was asked for.
 * \~spanish cuanto se puede escribir, que puede ser mas de lo que se pidio.
 * \~
 */
size_t malloc_usable_size(void *p) {
    if (p == nullptr) return 0;
    if (__builtin_expect(ours(p), 1)) return util::host_usable_size(p);
    if (vesta_interpose::from_bootstrap(p))
        return vesta_interpose::bootstrap_size(p);
    /* \~english Foreign, and here that has an honest answer: the C library's
     * own entry is still reachable because we did not overwrite it, only got
     * in front of it.  Zero if even that cannot be found -- which is what a
     * caller reads as "do not write anything", the only safe thing to say
     * about a block nobody can measure.
     *
     * \~spanish Ajeno, y aqui eso tiene respuesta honesta: la entrada de la
     * propia libreria de C sigue alcanzable porque no la pisamos, solo nos
     * pusimos delante.  Cero si ni eso se encuentra -- que es lo que quien
     * llama lee como "no escribas nada", lo unico seguro que se puede decir de
     * un bloque que nadie puede medir.  \~ */
    const UsableFn f = real_usable_size();
    return f != nullptr ? f(p) : 0;
}

} // extern "C"

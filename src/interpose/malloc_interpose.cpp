/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file util/malloc_interpose.cpp
 * @brief `malloc` and friends ARE this allocator, in C and in C++ alike.
 *
 * WHY THIS AND NOT A WRAPPER FUNCTION.  Because a wrapper only serves the code
 * that agrees to call it.  `operator new` is REPLACED, so every `new` in the
 * program -- ours, and the C++ libraries carried inside it -- already lands
 * here without anyone opting in.  The C entry points had no such thing, and
 * that hole is not only C's: a `.cpp` that calls `malloc` directly went to the
 * system just the same.  Handing those callers a `vesta_host_alloc` to call
 * instead would mean editing every one of them, and being wrong about the ones
 * nobody edits -- which are precisely the vendored ones this is meant to see.
 *
 * So `malloc`, `calloc`, `realloc` and `free` -- and the aligned entries next
 * to them -- are redirected AT LINK TIME with `-Wl,--wrap=`, and what they name
 * is this file.  The caller's code is not touched, not recompiled and not even
 * aware.
 *
 * WHAT THIS REACHES, AND WHAT IT DOES NOT.  It reaches every call in every
 * object of the link -- ours, and the static libraries built with the project.
 * It does NOT reach what the C runtime allocates INSIDE itself: those calls
 * never leave the runtime, so no linker can rename them.  That is a real hole
 * and it is written down here rather than left to be discovered: the report
 * would otherwise show that memory as zero, which reads like "there is none".
 *
 * `valloc` and `pvalloc` are the other two left out, and on purpose: they are
 * deprecated, they are the page-sized case of `memalign`, and nothing in the
 * link calls them.  If something ever does, its memory goes to the system and
 * does not show up here -- so they are named rather than silently missing.
 *
 * THE ALIGNED DOOR, AND WHY IT IS NOT THE SAME DOOR.  `posix_memalign`,
 * `aligned_alloc` and `memalign` are released with plain `free`, so the block
 * has to say what it is by ITSELF -- nobody is going to call a matching
 * `free_aligned`.  That is a question about the allocator's own layout, not
 * about this file, and it is answered there: `host_alloc_aligned_freeable`
 * serves it as a span, whose header `free` already finds by masking.  Writing a
 * second aligned allocator here -- a header, a magic, its own rounding -- would
 * be exactly the wrapper this file exists to avoid, one layer down.
 *
 * Windows is the other case and it is easier, because there the pairing is part
 * of the contract: `_aligned_malloc` is released with `_aligned_free`, never
 * with `free`.  Being told which is which, they use the cheap
 * `host_alloc_aligned`, which costs a few bytes instead of a chunk.
 *
 * THE PART THAT NEEDED CARE: FOREIGN BLOCKS.  `host_free` treats a pointer it
 * did not hand out as a hard error, and it is right to -- nothing else can
 * allocate, so such a pointer means somebody got in by another door and the
 * counts have quietly stopped adding up.  Interposing changes that: by the
 * time this is in force the runtime has already allocated during start-up, and
 * anything it returns for the caller to release (`strdup` and its family)
 * arrives here as a block we never made.  So THIS is the one place where a
 * foreign block is an expected case, and it goes back to the real `free`.  The
 * rule inside the allocator does not move an inch; what moved is the door.
 */

#include "interpose_common.h"

#include <cerrno>
#include <cstddef>

extern "C" {

/* What the linker leaves behind: the original entry points, still reachable
 * under these names.  They are needed for the blocks that are not ours.
 *
 * THE CORE FOUR ARE NOT AMONG THEM when `malloc_define.cpp` is in the build: it
 * defines those symbols, so the build removes their `--wrap`, and asking for a
 * `__real_` of a symbol nobody renamed is a name that does not exist.  Their
 * `__wrap_` twins go out with them, further down.
 *
 * They leave TOGETHER, and that is the part worth knowing: with `malloc` ours
 * and `realloc` still the C library's, the library allocates through us and
 * resizes through them, on a block whose header they never wrote.  Splitting
 * the family is not a smaller version of the mechanism, it is a corrupting one
 * -- see the backtrace in `malloc_define.cpp`. */
#if !defined(VESTA_ALLOC_DEFINE_MALLOC)
void *__real_malloc(size_t n);
void __real_free(void *p);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *p, size_t n);
#endif
/* There is no `__real__aligned_free`, and that is not an oversight: asking for
 * it makes the linker pull the member of `libmsvcrt.a` that defines the thunk,
 * and that same member defines `__imp__aligned_free` -- which is exactly the
 * symbol we define ourselves further down.  Two definitions of one name, and
 * the link fails.  See the Windows block. */

} // extern "C"

namespace {

/* `ours`, `pow2` and `note_site` used to live here.  They moved to
 * `interpose_common.h` when a SECOND way of becoming `malloc` appeared, and the
 * move was not tidying: `note_site` decides whether an allocation shows up in
 * the report at all, so two copies drifting apart would give a report that is
 * wrong without ever looking incomplete. */
using vesta_interpose::from_bootstrap;
using vesta_interpose::note_site;
using vesta_interpose::ours;
using vesta_interpose::pow2;

/**
 * @brief Whether this allocator is switched OFF and the call belongs to the one
 *        it replaced.
 *
 * WHEN THAT HAPPENS, now that there is no switch to turn it off: while it is
 * still DECIDING -- a window of one call, inside static initialisation, where a
 * request that reenters must not wait for a decision it would be making itself
 * -- and when it could not take over at all, because the region would not
 * reserve or a static link did not pull the objects in.
 *
 * WHY IT IS HANDLED HERE INSTEAD OF LEFT TO FAIL.  Renaming the calls happens at
 * LINK time and cannot be undone at run time, so a request arriving in either of
 * those states still comes through this door -- and `host_alloc` refuses when it
 * is not in force, with a refusal that does not return.  That is how the old
 * `VESTA_NO_HOST_SLAB` killed processes before `main`: the C runtime asked for
 * 105 bytes while starting up and there was nobody to serve it.  The switch is
 * gone; the two states above are not, and they are real.
 *
 * So the door stays renamed and the call is handed straight back.  `__real_*`
 * is exactly what `--wrap` leaves behind for this, and the free side already
 * worked this way -- see @c __wrap_free, where a block that is not ours goes to
 * `__real_free` and always did.
 *
 * IT IS ASKED ON EVERY CALL, and that is affordable HERE and would not be in
 * `host_alloc`: whoever arrives at these functions came through a call already,
 * so one predictable load off a line that never changes is lost in it.  The
 * fast path this library cares about does not come through here -- our own code
 * calls `util::host_alloc` inlined.
 */
[[gnu::always_inline]] inline bool stand_aside() noexcept {
    return !util::host_alloc_active();
}

} // namespace

extern "C" {

/* The four below only exist when nobody DEFINES those symbols.  When
 * `malloc_define.cpp` is in the build the renaming is removed for them -- the
 * two ways cannot coexist for one symbol, see `interpose_common.h` -- so a
 * `__wrap_` twin here would be code the linker points nothing at, next to a
 * `__real_` that does not exist.
 *
 * ONE guarded block and not four, because they go as a family: `malloc` and
 * `free` are two ends of the same promise, and `realloc` sits on both of them
 * at once. */
#if !defined(VESTA_ALLOC_DEFINE_MALLOC)

void *__wrap_malloc(size_t n) {
    if (__builtin_expect(stand_aside(), 0)) return __real_malloc(n);
    void *p = util::host_alloc(n);
    note_site(__builtin_return_address(0), n);
    return p;
}

void *__wrap_calloc(size_t count, size_t size) {
    /* The overflow of the product is the classic `calloc` bug, and an
     * expensive one: it allocates less than asked and the caller writes more.
     * Checked before multiplying. */
    if (count != 0 && size > (size_t(-1) / count)) return nullptr;
    if (__builtin_expect(stand_aside(), 0)) return __real_calloc(count, size);
    const size_t n = count * size;
    /* `host_alloc_zeroed` and not `host_alloc` plus a `memset`: memory that
     * has just come from the system is already zero, and clearing it again is
     * writing for nothing. */
    void *p = util::host_alloc_zeroed(n);
    note_site(__builtin_return_address(0), n);
    return p;
}

void *__wrap_realloc(void *p, size_t n) {
    /* Switched off: nothing here is ours, so the whole call belongs to the one
     * we replaced.  See @c stand_aside. */
    if (__builtin_expect(stand_aside(), 0)) return __real_realloc(p, n);
    /* A BOOTSTRAP block first, and it has to be first: it is not ours and it is
     * not the C library's either, so the test below would hand the library a
     * pointer it never made.  It cannot be grown in place -- that arena only
     * moves forward -- so it is copied out and left behind, which is what the
     * arena is for.  `getline` does exactly this during start-up, so it is not
     * a hypothetical path. */
    if (p != nullptr && from_bootstrap(p)) {
        void *fresh = util::host_alloc(n);
        if (fresh == nullptr) return nullptr; // `p` is still valid, as required
        const size_t old = vesta_interpose::bootstrap_size(p);
        vesta_memcpy(fresh, p, old < n ? old : n);
        note_site(__builtin_return_address(0), n);
        return fresh;
    }
    /* A block we did not hand out cannot be grown by us: we do not know how
     * big it is, and there is nowhere to ask.  It goes back to the allocator
     * that made it, whole. */
    if (p != nullptr && !ours(p)) return __real_realloc(p, n);
    void *q = util::host_realloc(p, n);
    /* A `realloc` is recorded too, and not as an afterthought: in C this is
     * how things grow -- it is the `std::vector` of this side -- so leaving it
     * out would hide the very pattern worth looking at.  With the NEW size,
     * which is what was just asked for. */
    note_site(__builtin_return_address(0), n);
    return q;
}

void __wrap_free(void *p) {
    if (p == nullptr) return;
    if (!ours(p)) {
        /* See the file header: here, and only here, this is normal.  The C
         * runtime allocated it before we were in force. */
        __real_free(p);
        return;
    }
    util::host_free(p);
}

#endif // !VESTA_ALLOC_DEFINE_MALLOC

/* --------------------------------------------------------------------------
 *  The aligned entries.  See the file header for why these are not `free`'s
 *  problem to solve: the block is served in a shape `free` ALREADY recognises.
 * ------------------------------------------------------------------------ */

/**
 * @brief `memalign`: the oldest of the three, and the loosest.
 *
 * It asks nothing of @p n and only that @p align be a power of two.  It is
 * deprecated everywhere and still called from vendored C, which is the whole
 * reason it is here.
 */
void *__wrap_memalign(size_t align, size_t n) {
    if (!pow2(align)) {
        errno = EINVAL;
        return nullptr;
    }
    void *p = util::host_alloc_aligned_freeable(n, align);
    if (p == nullptr) errno = ENOMEM;
    note_site(__builtin_return_address(0), n);
    return p;
}

/**
 * @brief `aligned_alloc`: the same thing, standardised in C11.
 *
 * The standard says @p n should be a multiple of @p align and nobody enforces
 * it -- glibc dropped the check because real code does not honour it.  Not
 * enforced here either: rejecting what every other allocator accepts would
 * break programs that are already working, and it would look like a bug in
 * this library rather than a rule being applied.
 */
void *__wrap_aligned_alloc(size_t align, size_t n) {
    if (!pow2(align)) {
        errno = EINVAL;
        return nullptr;
    }
    void *p = util::host_alloc_aligned_freeable(n, align);
    if (p == nullptr) errno = ENOMEM;
    note_site(__builtin_return_address(0), n);
    return p;
}

/**
 * @brief `posix_memalign`: the one that reports through its RETURN value.
 *
 * It does not touch `errno` and it does not return the pointer -- the two
 * things everybody gets wrong about it.  On failure @p out is left ALONE, which
 * is what POSIX says and what lets a caller keep whatever it had there.
 *
 * The extra demand over the other two is that @p align be a multiple of
 * `sizeof(void *)`; an alignment finer than a pointer is a caller mistake, and
 * saying so is better than quietly rounding it up.
 */
int __wrap_posix_memalign(void **out, size_t align, size_t n) {
    if (out == nullptr) return EINVAL;
    if (!pow2(align) || (align % sizeof(void *)) != 0) return EINVAL;
    void *p = util::host_alloc_aligned_freeable(n, align);
    if (p == nullptr) return ENOMEM;
    note_site(__builtin_return_address(0), n);
    *out = p;
    return 0;
}

#if defined(_WIN32)

/**
 * @brief
 * \~english Whether an ALIGNED pointer is one of ours, asked about the address
 *           that will actually be used.
 * \~spanish Si un puntero ALINEADO es nuestro, preguntado por la direccion que
 *           se va a usar de verdad.
 * \~
 *
 * \~english
 * WRITTEN ONCE BECAUSE FOUR ENTRIES NEED IT AND ALL FOUR HAD IT WRONG.
 * `_aligned_malloc` hands back an address raised to an alignment and keeps the
 * real one in the word before it; every entry in this family then acts on that
 * real one -- `host_free_aligned` releases `p[-1]`, `aligned_usable` measures
 * from `p[-1]` -- while all four ASKED `ours(p)`, about a different address
 * than the one they were about to use.
 *
 * It agreed until now because both sit inside the region, so the comparisons
 * said yes either way.  They stop agreeing the moment a block comes from
 * OUTSIDE it -- the checking mode's guard level does exactly that -- and then
 * these refused blocks they were about to handle correctly, and
 * `no_foreign_free` stopped the process.  Found with a debugger, twice: first
 * in `__wrap__aligned_free`, then, once that one was fixed, in
 * `__wrap__aligned_realloc`.  Fixing them one at a time is how the third and
 * fourth would have been found the same way, later, by somebody else.
 *
 * A question about one address and an action on another is a bug whether or not
 * anything notices, and this is the shape that hides it: the two agree for as
 * long as something else stays true elsewhere.
 *
 * \~spanish
 * ESCRITA UNA VEZ PORQUE LA NECESITAN CUATRO ENTRADAS Y LAS CUATRO LA TENIAN
 * MAL.  `_aligned_malloc` devuelve una direccion subida a una alineacion y
 * guarda la de verdad en la palabra de delante; todas las entradas de esta
 * familia actuan luego sobre esa de verdad -- `host_free_aligned` suelta
 * `p[-1]`, `aligned_usable` mide desde `p[-1]` -- mientras que las cuatro
 * PREGUNTABAN `ours(p)`, por una direccion distinta de la que iban a usar.
 *
 * Coincidia hasta ahora porque las dos caen dentro de la region, asi que las
 * comparaciones decian que si de cualquier forma.  Dejan de coincidir en cuanto
 * un bloque viene de FUERA -- el nivel de guarda del modo comprobacion hace
 * justo eso -- y entonces estas rechazaban bloques que iban a tratar bien, y
 * `no_foreign_free` paraba el proceso.  Encontrado con un depurador, dos veces:
 * primero en `__wrap__aligned_free` y, una vez arreglada esa, en
 * `__wrap__aligned_realloc`.  Arreglarlas de una en una es como la tercera y la
 * cuarta se habrian encontrado igual, mas tarde y por otro.
 *
 * Una pregunta sobre una direccion y una accion sobre otra es un fallo se note
 * o no, y esta es la forma que lo esconde: coinciden mientras siga siendo
 * cierta otra cosa en otro sitio.
 * \~
 */
/* \~english IT READS `p[-1]`, WHICH THE OLD TEST DID NOT.  Safe for anything
 * that can legitimately arrive here: these entries are only reached by a
 * pointer that came out of an `_aligned_malloc` -- ours or the runtime's -- and
 * both keep the base in the word before, so that word is inside a block the
 * caller owns.  The functions below all read it one line later anyway.  What
 * changes for a pointer that never came from there is the SHAPE of the
 * complaint, not whether there is one: it was already undefined to pass it.
 *
 * \~spanish LEE `p[-1]`, COSA QUE LA COMPROBACION VIEJA NO HACIA.  Es seguro
 * para todo lo que puede llegar aqui legitimamente: a estas entradas solo llega
 * un puntero salido de un `_aligned_malloc` -- nuestro o del runtime -- y los
 * dos guardan la base en la palabra de delante, asi que esa palabra esta dentro
 * de un bloque que el llamante posee.  Las funciones de abajo la leen igual una
 * linea despues.  Lo que cambia para un puntero que no vino de ahi es la FORMA
 * de la queja, no que la haya: pasarlo ya era indefinido.  \~ */
[[gnu::always_inline]] inline bool ours_aligned(void *p) noexcept {
    return p != nullptr && ours(((void **)p)[-1]);
}

/**
 * @brief `_aligned_malloc`: PAIRED, and that is what makes it cheap.
 *
 * Windows never releases one of these with `free` -- `_aligned_free` is part of
 * the contract -- so the block does not have to be self-describing and there is
 * no reason to spend a whole chunk on it.  `host_alloc_aligned` costs the
 * padding and eight bytes.
 *
 * The argument order is REVERSED from every other entry here (size first, then
 * alignment).  That is Microsoft's, not a slip.
 */
void *__wrap__aligned_malloc(size_t n, size_t align) {
    if (!pow2(align)) {
        errno = EINVAL;
        return nullptr;
    }
    void *p = util::host_alloc_aligned(n, align);
    if (p == nullptr) errno = ENOMEM;
    note_site(__builtin_return_address(0), n);
    return p;
}

/**
 * @brief The other half of the pair.
 *
 * Unlike `free`, a foreign block here is NOT an expected case, and so it gets
 * the treatment the allocator gives one everywhere else.  `free` has to forward
 * foreign blocks because the runtime allocates during start-up, before any of
 * this is in force, and hands them back later.  Nothing does that with
 * `_aligned_malloc`: both entries are replaced from the first instruction of
 * the process -- see the note below on how -- and the C runtime does not use
 * them internally.  A block arriving here that we did not make therefore means
 * somebody got in by another door, which is what `no_foreign_free` says.
 */
void __wrap__aligned_free(void *p) {
    if (p == nullptr) return;
    /* \~english ASKED ABOUT THE POINTER THIS IS GOING TO RELEASE, which is not
     * the one the caller holds.  `_aligned_malloc` hands back an address raised
     * to an alignment and keeps the real one in the word before it, so
     * `host_free_aligned` releases `p[-1]` -- and asking `ours(p)` was asking
     * about a different address than the one being acted on.
     *
     * It happened to agree until now, because both sit inside the region, so
     * the two comparisons said yes either way.  They stop agreeing the moment a
     * block is served from OUTSIDE the region -- the checking mode's guard
     * level does exactly that -- and then this refused a block it was about to
     * release correctly, and `no_foreign_free` stopped the process.  Found with
     * a debugger, in `no_foreign_free <- __wrap__aligned_free <- main`.
     *
     * A question about one address and an action on another is a bug whether or
     * not anything notices, and this is the shape that hides it: the two agree
     * for as long as one thing stays true elsewhere.
     *
     * \~spanish PREGUNTADO POR EL PUNTERO QUE ESTO VA A SOLTAR, que no es el
     * que tiene quien llama.  `_aligned_malloc` devuelve una direccion subida a
     * una alineacion y guarda la de verdad en la palabra de delante, asi que
     * `host_free_aligned` suelta `p[-1]` -- y preguntar `ours(p)` era preguntar
     * por una direccion distinta de aquella sobre la que se actua.
     *
     * Coincidian hasta ahora porque las dos caen dentro de la region, asi que
     * las dos comparaciones decian que si de cualquier forma.  Dejan de
     * coincidir en cuanto un bloque se sirve DESDE FUERA de la region -- el
     * nivel de guarda del modo comprobacion hace justo eso -- y entonces esto
     * rechazaba un bloque que iba a soltar bien, y `no_foreign_free` paraba el
     * proceso.  Encontrado con un depurador, en `no_foreign_free <-
     * __wrap__aligned_free <- main`.
     *
     * Una pregunta sobre una direccion y una accion sobre otra es un fallo se
     * note o no, y esta es la forma que lo esconde: coinciden mientras siga
     * siendo cierta otra cosa en otro sitio.  \~ */
    if (!ours_aligned(p)) util::detail::no_foreign_free(p); // does not return
    util::host_free_aligned(p);
}

/**
 * @brief How big an aligned block of ours is.
 *
 * The allocator knows the size of the RAW block, and the aligned pointer sits
 * somewhere inside it, so what is usable from there is the rest.  It is the
 * same arithmetic `_aligned_msize` has to do and the same `_aligned_realloc`
 * needs to know how much to copy -- written once.
 */
size_t aligned_usable(void *p) {
    void *raw = ((void **)p)[-1];
    const size_t whole = util::host_usable_size(raw);
    const size_t head = size_t((unsigned char *)p - (unsigned char *)raw);
    return whole > head ? whole - head : 0;
}

/**
 * @brief `_aligned_realloc`: the one that could still have corrupted memory.
 *
 * WHY IT IS HERE EVEN THOUGH NOBODY IN THIS TREE CALLS IT.  Because "nobody
 * calls it" is a property of today's code, and the failure it would cause is
 * not a crash at the call: `_aligned_realloc` would take a pointer this
 * allocator handed out and give it to the C runtime's heap, which would be
 * corruption discovered later and somewhere else.  A door that is only safe
 * while nobody walks through it is not safe.
 */
void *__wrap__aligned_realloc(void *p, size_t n, size_t align) {
    if (p == nullptr) return __wrap__aligned_malloc(n, align);
    if (!pow2(align)) {
        errno = EINVAL;
        return nullptr;
    }
    if (!ours_aligned(p)) util::detail::no_foreign_free(p); // does not return
    if (n == 0) {
        util::host_free_aligned(p);
        return nullptr;
    }
    /* No in-place growth: an aligned block is a raw one with the pointer
     * pushed up, and moving the boundary would move the alignment with it.
     * Fresh, copy, release -- which is what the runtime does anyway. */
    const size_t old = aligned_usable(p);
    if (old >= n) return p; // already fits; rounding plays in our favour
    void *fresh = util::host_alloc_aligned(n, align);
    if (fresh == nullptr) {
        errno = ENOMEM;
        return nullptr; // `p` stays valid, as the contract requires
    }
    vesta_memcpy(fresh, p, old);
    util::host_free_aligned(p);
    note_site(__builtin_return_address(0), n);
    return fresh;
}

/// `_aligned_recalloc`: grow and zero what is new.  Same reasoning as above.
void *__wrap__aligned_recalloc(void *p, size_t count, size_t size,
                               size_t align) {
    if (count != 0 && size > (size_t(-1) / count)) return nullptr;
    const size_t n = count * size;
    const size_t old = ours_aligned(p) ? aligned_usable(p) : 0;
    void *q = __wrap__aligned_realloc(p, n, align);
    /* Only the TAIL is cleared: the head came from the old block and clearing
     * it would throw away what the caller asked to keep. */
    if (q != nullptr && n > old)
        vesta_memset((unsigned char *)q + old, 0, n - old);
    return q;
}

/// `_aligned_msize`: how much the caller may use.  @p offset is the
/// `_aligned_offset_*` family's, and a non-zero one means a block this
/// allocator never made -- see below.
size_t __wrap__aligned_msize(void *p, size_t align, size_t offset) {
    (void)align;
    if (p == nullptr || offset != 0) {
        errno = EINVAL;
        return size_t(-1);
    }
    if (!ours_aligned(p)) util::detail::no_foreign_free(p); // does not return
    return aligned_usable(p);
}

/**
 * @brief The `_aligned_offset_*` family: REFUSED, loudly, and on purpose.
 *
 * They hand back a block where `p + offset` is the aligned address, not `p`.
 * This allocator has no such shape, and the honest answers are two: build it,
 * or say no.  Saying no is right for now -- nothing in this tree asks for it,
 * and a shape nobody uses is a shape nobody tests.
 *
 * What is NOT an option is leaving them alone.  Unwrapped, they would keep
 * going to msvcrt, and then its `_aligned_free` -- which IS wrapped -- would
 * be handed a block the C runtime made, or ours would be handed to theirs.
 * Refusing here keeps both allocators whole, and the caller finds out at the
 * call instead of somewhere else much later.
 */
void *__wrap__aligned_offset_malloc(size_t n, size_t align, size_t offset) {
    if (offset == 0) return __wrap__aligned_malloc(n, align);
    errno = EINVAL;
    return nullptr;
}

void *__wrap__aligned_offset_realloc(void *p, size_t n, size_t align,
                                     size_t offset) {
    if (offset == 0) return __wrap__aligned_realloc(p, n, align);
    errno = EINVAL;
    return nullptr;
}

/**
 * @brief
 * \~english `_msize`, which allocates nothing and still had to be taken.
 * \~spanish `_msize`, que no reserva nada y aun asi habia que cogerla.
 * \~
 *
 * \~english
 * WHY A FUNCTION THAT ONLY ASKS IS IN AN ALLOCATOR'S LIST.  Because it puts the
 * caller's pointer to the NT heap, and the heap checks: handed one of OUR
 * blocks it does not answer wrong, it stops the process with
 * STATUS_HEAP_CORRUPTION.  That is how a window died inside `CreateWindowExW`
 * -- uxtheme loads lazily, its start-up allocates an exit table through our
 * `malloc`, and `__dllonexit` then asks `_msize` how big it is.  Serving a
 * block and leaving the questions ABOUT it to somebody else is being `malloc`
 * by halves, and the halves are not only the verbs.
 *
 * THIS IS THE SECOND WAY IN, and it is not redundant with the first.  The patch
 * over `msvcrt!_msize` catches what the C runtime asks INSIDE itself; this
 * catches what our own link asks, and it is the only one left when the build
 * goes out without the patch (`VESTA_ALLOC_HOOK_MSVCRT=OFF`).  Two shapes of
 * call, two mechanisms -- the same split the aligned family below already has.
 *
 * A FOREIGN BLOCK GETS ZERO AND IS NOT PASSED ON, and here that IS a limit
 * rather than a choice: there can be no `__real__msize` to hand it to.  Asking
 * for one makes the linker pull the member of `libmsvcrt.a` that defines the
 * thunk, and that member also defines `__imp__msize` -- the symbol defined
 * below -- so the link fails on two definitions of one name.  The same trap as
 * `__real__aligned_free`; see the top of this file.
 *
 * \~spanish
 * POR QUE UNA FUNCION QUE SOLO PREGUNTA ESTA EN LA LISTA DE UN ASIGNADOR.
 * Porque le plantea al monton NT el puntero de quien llama, y el monton
 * comprueba: dandole uno de NUESTROS bloques no contesta mal, para el proceso
 * con STATUS_HEAP_CORRUPTION.  Asi murio una ventana dentro de
 * `CreateWindowExW` -- uxtheme se carga perezosamente, su arranque reserva una
 * tabla de salida por nuestro `malloc`, y `__dllonexit` le pregunta entonces a
 * `_msize` cuanto mide --.  Servir un bloque y dejarle a otro las preguntas
 * SOBRE el es ser `malloc` a medias, y las mitades no son solo los verbos.
 *
 * ESTA ES LA SEGUNDA VIA, y no sobra con la primera.  El parche sobre
 * `msvcrt!_msize` caza lo que el runtime de C pregunta POR DENTRO; esta caza lo
 * que pregunta nuestro propio enlace, y es la unica que queda cuando la
 * compilacion sale sin el parche (`VESTA_ALLOC_HOOK_MSVCRT=OFF`).  Dos formas
 * de llamada, dos mecanismos -- el mismo reparto que ya tiene la familia
 * alineada de mas abajo.
 *
 * UN BLOQUE AJENO RECIBE CERO Y NO SE REENVIA, y aqui eso SI es un limite y no
 * una eleccion: no puede existir un `__real__msize` al que darselo.  Pedirlo
 * hace que el enlazador saque el miembro de `libmsvcrt.a` que define el thunk,
 * y ese miembro define tambien `__imp__msize` -- el simbolo que se define mas
 * abajo --, asi que el enlace falla por dos definiciones de un nombre.  La
 * misma trampa que `__real__aligned_free`; ver la cabecera de este fichero.
 * \~
 */
size_t __wrap__msize(void *p) {
    return vesta_interpose::usable_bytes(p);
}

/**
 * @brief
 * \~english `_expand`, the other one that puts a pointer to the NT heap.
 * \~spanish `_expand`, la otra que le plantea un puntero al monton NT.
 * \~
 *
 * \~english
 * It calls `RtlSizeHeap` and then `RtlReAllocateHeap` with the in-place flag,
 * both on the caller's pointer -- the same two steps that killed the window,
 * one function over.  Which of msvcrt's entries needed taking was settled by
 * sweeping its code for references to the heap import slots rather than by
 * reading names: fourteen sites in six functions, of which these two were the
 * ones missing.  See `msvcrt_hook.cpp`, where the sweep is written down.
 *
 * \~spanish
 * Llama a `RtlSizeHeap` y luego a `RtlReAllocateHeap` con la bandera de hacerlo
 * en el sitio, las dos sobre el puntero de quien llama -- los mismos dos pasos
 * que mataron la ventana, una funcion mas alla.  Cuales de las entradas de
 * msvcrt habia que coger se decidio BARRIENDO su codigo en busca de
 * referencias a las ranuras de importacion del monton, y no leyendo nombres:
 * catorce sitios en seis funciones, de las cuales estas dos eran las que
 * faltaban.  Ver `msvcrt_hook.cpp`, donde queda escrito el barrido.
 * \~
 */
void *__wrap__expand(void *p, size_t n) {
    return vesta_interpose::expand_in_place(p, n);
}

/* --------------------------------------------------------------------------
 *  AND THE PART THAT MAKES EVERYTHING ABOVE REACHABLE AT ALL
 *
 *  `--wrap` renames PENDING references, and on Windows the reference is not
 *  pending under the name it would rename.  The CRT header declares these two
 *  `__declspec(dllimport)`, so a caller does not emit `call _aligned_malloc`:
 *  it emits an indirect call through `__imp__aligned_malloc`, a DATA symbol the
 *  loader fills in with the address inside msvcrt.dll.  Read off the
 *  relocations of a compiled object, not assumed:
 *
 *      __imp__aligned_malloc     <- dllimport: --wrap never sees this name
 *      __imp__aligned_free       <- the same
 *      malloc                    <- plain, which is why THAT one works
 *
 *  So we define the pointer ourselves.  The linker resolves the caller's
 *  reference with ours, never pulls the import library's member, and the
 *  indirect call lands here instead of in msvcrt -- at exactly the cost the
 *  call through the DLL already had, one indirect call.
 *
 *  WHY THE `--wrap` FLAGS STAY EVEN SO, which is the opposite of obvious: they
 *  are what keeps this LINKING.  A plain reference to `_aligned_free` -- from a
 *  caller that declared the symbol by hand instead of including the header --
 *  would pull the import library's member, and that member also defines
 *  `__imp__aligned_free`, which we have just defined.  Two definitions, and the
 *  link fails.  With the flag in place that reference is renamed to
 *  `__wrap__aligned_free` and the member is never pulled.  The two mechanisms
 *  cover the two shapes a call can have, and neither one is redundant.
 *
 *  THE WHOLE FAMILY IS HERE, and it used to be only the first two.  That was
 *  the one way this could still corrupt: `_aligned_realloc` and its relatives
 *  would be handed one of OUR pointers and pass it to msvcrt's heap -- damage
 *  found later and somewhere else.  "Nothing in this tree calls them" was true
 *  and is not a property to build on: a door is not safe because nobody has
 *  walked through it yet.
 * ------------------------------------------------------------------------ */
void *(*__imp__aligned_malloc)(size_t, size_t) = &__wrap__aligned_malloc;
void (*__imp__aligned_free)(void *) = &__wrap__aligned_free;
void *(*__imp__aligned_realloc)(void *, size_t, size_t) =
    &__wrap__aligned_realloc;
void *(*__imp__aligned_recalloc)(void *, size_t, size_t, size_t) =
    &__wrap__aligned_recalloc;
size_t (*__imp__aligned_msize)(void *, size_t, size_t) =
    &__wrap__aligned_msize;
void *(*__imp__aligned_offset_malloc)(size_t, size_t, size_t) =
    &__wrap__aligned_offset_malloc;
void *(*__imp__aligned_offset_realloc)(void *, size_t, size_t, size_t) =
    &__wrap__aligned_offset_realloc;
/* \~english AND THE TWO THAT ONLY ASK, which need this every bit as much: the
 * relocations of a compiled object show `__imp__msize` and `__imp__expand`,
 * never the plain names, so the renaming alone would rename nothing and the
 * wrappers above would be code nobody reaches.  Checked the same way as the
 * rest of this block, by compiling a caller and reading what it emitted.
 *
 * \~spanish Y LAS DOS QUE SOLO PREGUNTAN, que lo necesitan igual: las
 * reubicaciones de un objeto compilado ensenan `__imp__msize` y
 * `__imp__expand`, nunca los nombres pelados, asi que el renombrado por si solo
 * no renombraria nada y los envoltorios de arriba serian codigo al que no llega
 * nadie.  Comprobado igual que el resto de este bloque, compilando un llamante
 * y leyendo lo que emitio.  \~ */
size_t (*__imp__msize)(void *) = &__wrap__msize;
void *(*__imp__expand)(void *, size_t) = &__wrap__expand;

#endif // _WIN32

} // extern "C"

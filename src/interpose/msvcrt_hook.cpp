/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file src/msvcrt_hook.cpp
 * @brief The Windows half of "the C runtime allocates from us too".
 *
 * WHAT IS LEFT OVER ON WINDOWS.  Renaming at link time reaches every call in
 * the link, and the `__imp__` pointers reach the ones that go through the
 * import table.  Neither reaches what msvcrt allocates INSIDE itself and hands
 * back for the caller to release -- `_strdup`, `_wcsdup`, `_fullpath`,
 * `_getcwd`.  Those are calls from one function of the DLL to another function
 * of the same DLL: they never leave it, so there is no reference to rename and
 * no import entry to point elsewhere.
 *
 * On ELF the same hole is closed by DEFINING the symbol, because a shared
 * library's own calls go out through the PLT.  A DLL has no PLT: an internal
 * call is a relative call straight to the code.  So the only place left to
 * intervene is the code itself, and the intervention is one instruction -- a
 * jump at the entry of `malloc` pointing at ours.  Same machinery as the patch
 * this library already writes over `operator new`, shared in `code_patch.h`.
 *
 * ONLY msvcrt.  Nothing here touches kernel32, ntdll or any other component:
 * the C runtime is the one that allocates on the program's behalf.
 *
 * ------------------------------------------------------------------------
 * WHEN THE JUMPS GO IN, AND WHY THAT IS THE WHOLE DESIGN
 * ------------------------------------------------------------------------
 *
 * They go in from a TLS callback, which runs BEFORE the process entry point --
 * before the C runtime starts, before any constructor.  That is not a detail:
 * every block the runtime allocates before the patch is a block this allocator
 * did not make and cannot account for, and the earlier the jumps go in, the
 * fewer of those exist.  Measured on this machine:
 *
 *     TLS callback        9 blocks already live
 *     constructor(101)   13
 *     ordinary ctor      13
 *
 * The four that separate the two is not an abstract improvement.  One of them
 * is the `atexit` table, and `_onexit` GROWS IT WITH `realloc` -- so patching
 * late meant a `realloc` arriving here for a block that was not ours, on a heap
 * that was not ours either.  That is what a first version did, by asking the
 * runtime's heap how big the block was, and the system stopped the process for
 * heap corruption: the address did not belong to the heap that was asked.
 *
 * ------------------------------------------------------------------------
 * AND NO HEAP OF ANYBODY ELSE'S IS TOUCHED
 * ------------------------------------------------------------------------
 *
 * There is no `HeapSize` and no `HeapFree` here any more, and that is the rule
 * rather than the fix: this allocator gets its memory from the system with
 * `VirtualAlloc` and manages its own, and reaching into a heap somebody else
 * owns is administering an allocator we do not run.  Guessing which heap a
 * foreign pointer came from is exactly what corrupted it.
 *
 * So a block that is not ours is LEFT ALONE, and counted.  Nine of them, made
 * before the first instruction of the program: a bounded number that does not
 * grow, and most of which lives until the process ends anyway.  Counted, not
 * ignored -- a leak nobody measures cannot be told apart from no leak.
 *
 * ------------------------------------------------------------------------
 * WHICH FUNCTIONS, AND HOW THAT LIST WAS ARRIVED AT
 * ------------------------------------------------------------------------
 *
 * Not by picking the ones whose names sound like memory.  The section of code
 * of msvcrt.dll was swept for every reference to the heap import slots, which
 * is where a pointer actually reaches the NT heap.  There are FOURTEEN such
 * sites, in six functions:
 *
 *     malloc  calloc  realloc  free      the ones that allocate
 *     _msize  _expand                    the ones that only ASK
 *     _heapchk  _heapwalk  _chsize       never see a pointer of ours
 *
 * The last three walk that heap, or use it for a buffer they make and release
 * themselves; nothing of ours can arrive at them.  The other six are the list,
 * and the middle two are the ones this file was missing: they allocate nothing
 * at all, so they do not look like an allocator's business, and both hand the
 * caller's pointer to `RtlSizeHeap`.  That is what stopped a process inside
 * `CreateWindowExW` -- see `vesta_crt_msize`, which carries the trace.
 *
 * Everything else in the family reaches the heap THROUGH those six and needs
 * no jump of its own: `_aligned_malloc` and `_aligned_offset_malloc` call
 * `malloc`, `_aligned_free` calls `free`, `_aligned_realloc` goes through
 * `_msize` and `_expand`, `_strdup` and `_getcwd` allocate with `malloc`, and
 * `_msize_dbg` and `_expand_dbg` are one-instruction jumps into the real ones.
 * Patching a function that merely calls a patched function would destroy
 * fourteen bytes of somebody else's prologue to change nothing.
 */

#if defined(_WIN32)

#include "code_patch.h"
#include "interpose_common.h"
#include "util/interpose/msvcrt_hook.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <windows.h>

namespace {

/* EL SALTO LARGO, y no el corto de cinco bytes que usa el parche de
 * `operator new`.  Ese alcanza dos gibibytes, que sobra dentro del propio
 * modulo y NO llega a msvcrt.dll: el sistema la carga donde quiere y con
 * aleatorizacion de direcciones la distancia se sale de un desplazamiento de 32
 * bits con toda normalidad.  No es teorico -- la primera version usaba el corto
 * y los cuatro saltos se negaron a entrar, cada uno diciendolo. */
using util::patch_detail::write_jump_far;
using vesta_interpose::note_site;
using vesta_interpose::ours;

/// Blocks the runtime made before the patch that came back to us.  Left where
/// they are; see the file header.
std::atomic<unsigned long long> g_foreign{0};
/// Whether the four jumps went in.
std::atomic<bool> g_hooked{false};

/* --------------------------------------------------------------------------
 *  LA LISTA DE SALIDA, PROPIA -- y por que resuelve el unico caso que quedaba
 *
 *  El `realloc` ajeno que se colaba no venia de ningun sitio raro: venia de
 *  NOSOTROS.  El informe del asignador se imprime desde un destructor de un
 *  objeto estatico, y en esta cadena de herramientas eso se registra por la
 *  maquinaria de `atexit` del runtime, que guarda las funciones en una tabla
 *  y la HACE CRECER con `realloc`.  Esa tabla nacio antes del parche, asi que
 *  el realloc llegaba aqui con un bloque que no era nuestro.
 *
 *  Con el registro apuntando a esta lista, esa tabla no vuelve a crecer nunca:
 *  no hay reserva, no hay realloc, y no queda ningun bloque ajeno en el camino
 *  caliente.  La lista vive en `.bss` -- no reserva NADA, que es la condicion
 *  para poder estar en funcionamiento antes que el asignador.
 * ------------------------------------------------------------------------ */

/// Cuantas funciones de salida caben.  Un programa registra unas pocas
/// decenas; pasarse se DICE en vez de perder la funcion en silencio.
constexpr unsigned kExitSlots = 512;

using ExitFn = void(__cdecl *)(void);
ExitFn g_exit_list[kExitSlots];
std::atomic<unsigned> g_exit_count{0};

/**
 * @brief Ejecuta lo registrado, en orden inverso, como manda `atexit`.
 *
 * IDEMPOTENTE a proposito, y no por elegancia: se llama desde dos sitios -- la
 * plaza que se pide en la tabla del runtime, que es la buena, y el desmontaje
 * del proceso, que es la red por si aquella no se pudo pedir --.  El
 * intercambio a cero deja la lista vacia en la primera, asi que la segunda no
 * repite nada.
 */
void __cdecl run_exit_list() noexcept {
    unsigned n = g_exit_count.exchange(0, std::memory_order_acq_rel);
    if (n > kExitSlots) n = kExitSlots;
    while (n-- > 0)
        if (g_exit_list[n] != nullptr) g_exit_list[n]();
}

/**
 * @brief
 * \~english Says, once, that something arrived here that cannot be served.
 * \~spanish Dice, una vez, que llego aqui algo que no se puede servir.
 * \~
 *
 * \~english
 * Once and not every time: this runs on a path the runtime may be walking
 * during start-up or shutdown, and a message per event would turn a bounded
 * oddity into a flood that hides everything else.  The COUNT keeps the
 * magnitude; the message is only there so nobody has to go looking for it.
 *
 * ONCE PER REASON, and the flag comes from the caller for that alone.  It used
 * to be a single flag shared by the whole file, so the first thing to happen
 * silenced every other reason for the rest of the process -- and the reasons
 * are what this function exists to say.  A diagnostic that reports the first
 * cause and hides the second is the failure mode this library chases
 * everywhere else.
 *
 * \~spanish
 * Una vez y no cada vez: esto corre por un camino que el runtime puede estar
 * recorriendo al arrancar o al cerrar, y un mensaje por evento convertiria una
 * rareza acotada en una riada que tapa todo lo demas.  La CUENTA guarda la
 * magnitud; el mensaje solo esta para que nadie tenga que ir a buscarlo.
 *
 * UNA VEZ POR MOTIVO, y la bandera la trae quien llama solo para eso.  Antes
 * era una sola bandera compartida por el fichero entero, asi que lo primero que
 * pasara callaba todos los demas motivos durante el resto del proceso -- y los
 * motivos son justo lo que esta funcion existe para decir.  Un diagnostico que
 * avisa de la primera causa y esconde la segunda es el modo de fallo que esta
 * libreria persigue en todo lo demas.
 * \~
 */
void say_once(std::atomic<bool> &said, const char *what) {
    if (said.exchange(true, std::memory_order_relaxed)) return;
    std::fprintf(stderr, "[allocator] %s\n", what);
}

} // namespace

extern "C" {

/* The four the runtime calls internally.  They are what the jumps point at, so
 * they carry C linkage and plain names: what arrives here came from inside
 * msvcrt, which knows nothing about us. */

void *vesta_crt_malloc(size_t n) {
    void *p = util::host_alloc(n);
    note_site(__builtin_return_address(0), n);
    return p;
}

void *vesta_crt_calloc(size_t count, size_t size) {
    if (count != 0 && size > (size_t(-1) / count)) return nullptr;
    const size_t n = count * size;
    void *p = util::host_alloc_zeroed(n);
    note_site(__builtin_return_address(0), n);
    return p;
}

/**
 * @brief `realloc`, and the one case that has no honest answer.
 *
 * A block of ours grows the ordinary way.  A block from BEFORE the patch cannot
 * be grown, and not for lack of trying: copying it needs its old size, its old
 * size only its own heap knows, and asking a heap about a pointer that may not
 * be its own is what corrupted this the first time round.
 *
 * So it returns null, which is what `realloc` says when it cannot grow -- and
 * the caller keeps its block intact, which the standard guarantees and which is
 * the only reason this is safe rather than merely quiet.  It is also SAID: a
 * silent null here would look like running out of memory.
 *
 * With the jumps going in from a TLS callback this should not happen at all --
 * the table that used to arrive here is allocated after the patch now, so it is
 * ours.  The path stays because "should not happen" is not a thing to leave
 * unhandled in a function that would otherwise corrupt memory.
 */
void *vesta_crt_realloc(void *p, size_t n) {
    if (p != nullptr && !ours(p)) {
        g_foreign.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<bool> said{false};
        say_once(said,
                 "a `realloc` arrived for a block the C runtime made before "
                 "this allocator was in force. Its size is only known to the "
                 "heap that made it, and guessing is what corrupts memory, so "
                 "the call reports failure and the block is left untouched.");
        return nullptr;
    }
    /* \~english NOTE for whoever follows the `_msize` trail below: this entry
     * is what `__dllonexit` uses to GROW the table whose size it just asked
     * for.  It reaches here -- checked in the disassembly, msvcrt's internal
     * `realloc` helper calls the exported entry, which is the one patched --
     * so the pair "ask the size, then grow" stays inside this allocator from
     * end to end.
     *
     * \~spanish NOTA para quien siga el rastro de `_msize` de mas abajo: esta
     * entrada es la que usa `__dllonexit` para HACER CRECER la tabla cuyo
     * tamano acaba de preguntar.  Llega aqui -- comprobado en el
     * desensamblado, el ayudante interno de `realloc` de msvcrt llama a la
     * entrada exportada, que es la parcheada --, asi que el par "preguntar el
     * tamano y crecer" se queda dentro de este asignador de punta a punta.
     * \~ */
    void *q = util::host_realloc(p, n);
    note_site(__builtin_return_address(0), n);
    return q;
}

/**
 * @brief
 * \~english `_msize`, which is the door the heap corruption came through.
 * \~spanish `_msize`, que es la puerta por la que entraba la corrupcion del
 *           monton.
 * \~
 *
 * \~english
 * HOW IT WAS FOUND, because the report named nobody.  A window died inside
 * `CreateWindowExW` with `0xC0000374`, STATUS_HEAP_CORRUPTION, and the frames
 * were bare offsets: `uxtheme +0x28480 ... msvcrt +0x3a553 ... msvcrt +0x2a8e0
 * ... ntdll +0x2422f`.  Resolved against the binaries they are `_initterm
 * +0x43`, `__dllonexit +0x30` and `RtlSizeHeap +0xcf` -- and `__dllonexit
 * +0x30` is the instruction AFTER `call _msize`, so `_msize` is the one in the
 * middle.  It was read off the disassembly, not guessed from the names.
 *
 * WHAT WAS HAPPENING.  uxtheme is loaded LAZILY, from inside
 * `CreateWindowExW`, long after the jumps went in -- so the atexit table its
 * start-up code allocates with `malloc` is OURS.  Then `__dllonexit` asks how
 * big that table is, and `_msize` hands our pointer to `RtlSizeHeap`, which
 * reads NT heap metadata that nobody ever wrote.  The system is right to stop
 * the process: the block really is not in that heap.
 *
 * This is the Windows twin of the ELF bug: being `malloc` by halves corrupts.
 * Serving the allocation and leaving the question about it to somebody else is
 * the same mistake as serving `malloc` and leaving `realloc` to the C library.
 *
 * THE ANSWER ITSELF IS NOT HERE.  It is @c vesta_interpose::usable_bytes, next
 * to the reason a foreign block gets zero rather than the `-1` msvcrt would
 * give -- and it is shared because the renamed `__wrap__msize` has to answer
 * the same thing.  What stays here is the counting and the message, which are
 * this door's and not the answer's.
 *
 * \~spanish
 * COMO SE ENCONTRO, porque el informe no nombraba a nadie.  Una ventana moria
 * dentro de `CreateWindowExW` con `0xC0000374`, STATUS_HEAP_CORRUPTION, y los
 * marcos eran desplazamientos pelados: `uxtheme +0x28480 ... msvcrt +0x3a553
 * ... msvcrt +0x2a8e0 ... ntdll +0x2422f`.  Resueltos contra los binarios son
 * `_initterm +0x43`, `__dllonexit +0x30` y `RtlSizeHeap +0xcf` -- y
 * `__dllonexit +0x30` es la instruccion de DESPUES de `call _msize`, asi que
 * `_msize` es la de en medio.  Salio de leer el desensamblado, no de adivinar
 * por los nombres.
 *
 * QUE PASABA.  uxtheme se carga PEREZOSAMENTE, desde dentro de
 * `CreateWindowExW`, mucho despues de que entraran los saltos -- asi que la
 * tabla de salida que su codigo de arranque reserva con `malloc` es NUESTRA.
 * Entonces `__dllonexit` pregunta cuanto mide esa tabla, y `_msize` le da
 * nuestro puntero a `RtlSizeHeap`, que lee metadatos del monton NT que nadie
 * escribio nunca.  El sistema hace bien en parar el proceso: el bloque
 * realmente no esta en ese monton.
 *
 * Es el gemelo en Windows del fallo de ELF: ser `malloc` a medias corrompe.
 * Servir la reserva y dejarle a otro la pregunta SOBRE ella es la misma
 * equivocacion que servir `malloc` y dejarle `realloc` a la libreria de C.
 *
 * LA RESPUESTA EN SI NO ESTA AQUI.  Es @c vesta_interpose::usable_bytes, junto
 * al motivo de que un bloque ajeno reciba cero y no el `-1` que daria msvcrt --
 * y se comparte porque el renombrado `__wrap__msize` tiene que contestar lo
 * mismo.  Lo que se queda aqui es la cuenta y el mensaje, que son de esta
 * puerta y no de la respuesta.
 * \~
 *
 * @param p
 * \~english a block, ours or not.
 * \~spanish un bloque, nuestro o no.
 * \~
 * @return
 * \~english usable bytes, which may be more than were asked for; 0 for null or
 *           for a block this allocator did not make.
 * \~spanish bytes utilizables, que pueden ser mas de los que se pidieron; 0 si
 *           es nulo o si el bloque no lo hizo este asignador.
 * \~
 */
size_t vesta_crt_msize(void *p) {
    if (p == nullptr || ours(p)) return vesta_interpose::usable_bytes(p);
    g_foreign.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<bool> said{false};
    say_once(said,
             "an `_msize` arrived for a block the C runtime made before this "
             "allocator was in force. Only the heap that made it knows its "
             "size, and asking that heap about a pointer that might not be "
             "its own is what stops the process for heap corruption, so the "
             "call answers zero and the caller grows the block instead.");
    return 0;
}

/**
 * @brief
 * \~english `_expand`, the other one that asks the NT heap about a pointer.
 * \~spanish `_expand`, la otra que le pregunta al monton NT por un puntero.
 * \~
 *
 * \~english
 * IT IS HERE FOR THE SAME REASON AND NOT BECAUSE OF A CRASH.  Sweeping msvcrt's
 * whole code section for references to the heap import slots turns up fourteen
 * sites in six functions: `malloc`, `calloc`, `realloc` and `free`, which are
 * already patched; `_heapchk`, `_heapwalk` and `_chsize`, which walk that heap
 * or use it for a buffer of their own and never see a pointer of ours; and
 * these two.  `_expand` calls `RtlSizeHeap` and then `RtlReAllocateHeap` with
 * the in-place flag, both on the caller's pointer -- the same two steps that
 * killed the window, one function over.  With these two in, the family is
 * CLOSED: the aligned entries and the `_dbg` variants reach the heap only
 * through `malloc`, `free`, `_msize` and `_expand`, so nothing else needs a
 * jump written over it.
 *
 * WHAT IT ANSWERS is @c vesta_interpose::expand_in_place, shared with the
 * renamed twin for the same reason as above.
 *
 * \~spanish
 * ESTA AQUI POR LO MISMO Y NO POR UNA CAIDA.  Barrer la seccion de codigo
 * entera de msvcrt buscando referencias a las ranuras de importacion del monton
 * da catorce sitios en seis funciones: `malloc`, `calloc`, `realloc` y `free`,
 * ya parcheadas; `_heapchk`, `_heapwalk` y `_chsize`, que recorren ese monton o
 * lo usan para un buffer propio y no ven jamas un puntero nuestro; y estas dos.
 * `_expand` llama a `RtlSizeHeap` y luego a `RtlReAllocateHeap` con la bandera
 * de hacerlo en el sitio, las dos sobre el puntero de quien llama -- los mismos
 * dos pasos que mataron la ventana, una funcion mas alla.  Con estas dos
 * puestas la familia queda CERRADA: las entradas alineadas y las variantes
 * `_dbg` solo llegan al monton por `malloc`, `free`, `_msize` y `_expand`, asi
 * que no hace falta escribir un salto encima de ninguna otra.
 *
 * QUE CONTESTA es @c vesta_interpose::expand_in_place, compartida con el
 * gemelo renombrado por lo mismo que arriba.
 * \~
 *
 * @param p
 * \~english the block to resize in place.
 * \~spanish el bloque a redimensionar en el sitio.
 * \~
 * @param n
 * \~english the size wanted.
 * \~spanish el tamano que se quiere.
 * \~
 * @return
 * \~english @p p when it already holds @p n bytes, null otherwise.
 * \~spanish @p p cuando ya tiene @p n bytes, nulo si no.
 * \~
 */
void *vesta_crt_expand(void *p, size_t n) {
    if (p == nullptr || ours(p)) return vesta_interpose::expand_in_place(p, n);
    g_foreign.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<bool> said{false};
    say_once(said,
             "an `_expand` arrived for a block the C runtime made before this "
             "allocator was in force. Growing it where it lies would mean "
             "asking a heap about a pointer that might not be its own, so the "
             "call reports it could not, which is what `_expand` says when it "
             "cannot and what every caller already handles.");
    return nullptr;
}

/**
 * @brief `atexit`, apuntando a nuestra lista en vez de a la tabla del runtime.
 *
 * No es capricho ni ganas de reimplementar la biblioteca: es lo que quita del
 * medio el ULTIMO bloque ajeno que quedaba en un camino caliente.  La tabla del
 * runtime crece con `realloc`, nacio antes del parche, y no hay forma honesta
 * de hacerle un `realloc` sin preguntarle a su heap.  Con el registro aqui, esa
 * tabla no crece jamas.
 *
 * Devuelve cero al acertar, como manda el estandar -- que es al reves que casi
 * todo lo demas, y por eso se escribe explicito.
 */
int __cdecl vesta_crt_atexit(ExitFn fn) {
    if (fn == nullptr) return -1;
    const unsigned i = g_exit_count.fetch_add(1, std::memory_order_acq_rel);
    if (i >= kExitSlots) {
        static std::atomic<bool> said{false};
        say_once(said,
                 "more exit functions were registered than the list holds; "
                 "the ones past the limit will not run. Raise kExitSlots.");
        return -1;
    }
    g_exit_list[i] = fn;
    return 0;
}

/**
 * @brief `_onexit`, que es la misma cosa con otra firma y otro valor devuelto.
 *
 * Es la de dentro: `atexit` del runtime no es mas que una capa sobre ella, y lo
 * que registra un destructor estatico acaba aqui.  Devuelve la funcion al
 * acertar y nulo al fallar, justo lo contrario de `atexit`.
 */
using OnExitFn = int(__cdecl *)(void);

OnExitFn __cdecl vesta_crt_onexit(OnExitFn fn) {
    /* La firma difiere en el tipo devuelto de la funcion registrada, no en como
     * se llama: se guarda tal cual y se invoca ignorando lo que devuelva, que
     * es lo que hace el runtime. */
    return vesta_crt_atexit(reinterpret_cast<ExitFn>(fn)) == 0 ? fn : nullptr;
}

void vesta_crt_free(void *p) {
    if (p == nullptr) return;
    if (ours(p)) {
        util::host_free(p);
        return;
    }
    /* From before the patch.  LEFT WHERE IT IS, and counted.  It cannot go back
     * to msvcrt's `free` -- that entry is patched and would come straight back
     * here -- and it will not be handed to a heap we merely suspect it came
     * from.  See the file header. */
    g_foreign.fetch_add(1, std::memory_order_relaxed);
}

/* --------------------------------------------------------------------------
 *  THE ENVIRONMENT, which turned out to be the same problem wearing a hat.
 *
 *  `_putenv_s` grows the runtime's environment table with `realloc`.  That
 *  table was built when msvcrt.dll loaded -- before our jumps go in -- so it
 *  arrives at `vesta_crt_realloc` as a foreign block, which cannot be grown
 *  because only its own heap knows its size.  It reported failure and the
 *  runtime SWALLOWED it: measured on twenty-four variables in a row,
 *  `_putenv_s` returned 0 for success every time and not one of them existed
 *  afterwards -- not to `getenv`, not in the process environment block, not to
 *  this library's own reader.  A write that returns success and does nothing is
 *  the worst failure this project admits, so it is not left to a diagnostic.
 *
 *  THE FIX IS THE SAME ONE AS EVERYWHERE ELSE IN THIS FILE: our own version.
 *  And it settles a question that was already half-settled -- WHICH environment
 *  is the real one.  This library has always read the block the KERNEL keeps
 *  (`util/os/os_env.h`), never `getenv`, precisely because the runtime's copy is
 *  a copy.  So these make the block the one and only answer: writes go to it,
 *  reads come from it, and the runtime's private table stops being consulted by
 *  anybody who comes through here.
 *
 *  WHAT THIS DOES NOT COVER, said rather than discovered later: code that walks
 *  the `_environ` array itself instead of calling `getenv` still sees the
 *  runtime's copy, and that copy no longer moves.  Nothing in this project does
 *  that; a third party might.
 * -------------------------------------------------------------------------- */

/**
 * @brief One remembered answer for @c getenv.
 *
 * WHY ANYTHING IS REMEMBERED AT ALL.  `getenv` hands back a pointer the caller
 * may keep and read, so the bytes have to outlive the call -- and the process
 * block cannot be pointed into, because it is UTF-16 and `getenv` is narrow.
 * So the value is converted once per lookup into storage that belongs to this
 * table, and the pointer stays good until the same name is asked for again,
 * which is exactly the lifetime the C standard grants.
 *
 * The buffer only ever GROWS, and the old one is never released: another thread
 * may still be reading through a pointer we handed out, and there is no way to
 * know.  A few dozen bytes per variable that outgrew its slot, once, is a price
 * worth paying for not handing out a dangling pointer.
 */
struct EnvAnswer {
    EnvAnswer *next;
    char *name;
    char *value;
    size_t value_cap;
};

std::atomic<EnvAnswer *> g_env_head{nullptr};
/// Guards the list.  A plain flag, spun on: looking up the environment is rare
/// and never on a hot path, so there is nothing here worth a real lock.
std::atomic<bool> g_env_busy{false};

void env_lock() noexcept {
    while (g_env_busy.exchange(true, std::memory_order_acquire))
        Sleep(0);
}
void env_unlock() noexcept { g_env_busy.store(false, std::memory_order_release); }

/// ASCII case-insensitive compare, because environment names on Windows are
/// case-insensitive and `getenv` is expected to behave that way.  Written out
/// rather than taken from `<cstring>`: this runs before the runtime is fully up.
bool env_name_equal(const char *a, const char *b) noexcept {
    for (;; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
        if (x != y) return false;
        if (x == '\0') return true;
    }
}

size_t env_len(const char *s) noexcept {
    size_t n = 0;
    while (s[n] != '\0') ++n;
    return n;
}

/**
 * @brief `getenv`, answered from the process environment block.
 *
 * @param name The variable to look up.
 * @return A pointer good until the same name is asked for again, or nullptr if
 *         the variable is not set.
 */
char *vesta_crt_getenv(const char *name) {
    if (name == nullptr || name[0] == '\0') return nullptr;

    /* With a zero-length buffer this returns the size NEEDED, nul included, and
     * zero when the variable is not there.  Two calls rather than a guessed
     * buffer: a guess that is too small truncates, and a truncated environment
     * variable is a wrong answer that looks like a right one. */
    const DWORD need = GetEnvironmentVariableA(name, nullptr, 0);
    if (need == 0) return nullptr;

    env_lock();
    EnvAnswer *e = g_env_head.load(std::memory_order_relaxed);
    while (e != nullptr && !env_name_equal(e->name, name)) e = e->next;

    if (e == nullptr) {
        e = static_cast<EnvAnswer *>(util::host_alloc(sizeof(EnvAnswer)));
        if (e == nullptr) {
            env_unlock();
            return nullptr;
        }
        const size_t n = env_len(name);
        e->name = static_cast<char *>(util::host_alloc(n + 1));
        if (e->name == nullptr) {
            util::host_free(e);
            env_unlock();
            return nullptr;
        }
        for (size_t i = 0; i <= n; ++i) e->name[i] = name[i];
        e->value = nullptr;
        e->value_cap = 0;
        e->next = g_env_head.load(std::memory_order_relaxed);
        g_env_head.store(e, std::memory_order_release);
    }

    if (e->value_cap < need) {
        char *bigger = static_cast<char *>(util::host_alloc(need));
        if (bigger == nullptr) {
            env_unlock();
            return nullptr;
        }
        /* The old buffer is NOT released; see `EnvAnswer`. */
        e->value = bigger;
        e->value_cap = need;
    }
    const DWORD got = GetEnvironmentVariableA(name, e->value, DWORD(e->value_cap));
    env_unlock();
    /* `got` is the length without the nul, and it must fit what was just asked
     * for.  If the variable changed size between the two calls -- another thread
     * writing it -- this says no rather than returning half of it. */
    return (got != 0 && got < e->value_cap) ? e->value : nullptr;
}

/**
 * @brief `_putenv_s`, writing the process environment block.
 *
 * @param name  The variable.  An empty value REMOVES it, which is what both
 *              this function and `SetEnvironmentVariableA` already meant.
 * @param value What to set it to.
 * @return 0 on success, EINVAL on a name this cannot mean.
 */
int vesta_crt_putenv_s(const char *name, const char *value) {
    if (name == nullptr || name[0] == '\0') return EINVAL;
    for (const char *p = name; *p != '\0'; ++p)
        if (*p == '=') return EINVAL; // a name cannot contain the separator
    const bool remove = value == nullptr || value[0] == '\0';
    return SetEnvironmentVariableA(name, remove ? nullptr : value) ? 0 : EINVAL;
}

/**
 * @brief `_putenv`, which is the same thing with the pair already joined.
 *
 * @param nameval `NAME=VALUE`, or `NAME=` to remove.
 * @return 0 on success, -1 on failure, which is what this function returns.
 */
int vesta_crt_putenv(const char *nameval) {
    if (nameval == nullptr) return -1;
    const char *eq = nameval;
    while (*eq != '\0' && *eq != '=') ++eq;
    if (*eq != '=') return -1; // no separator: nothing to set

    const size_t n = size_t(eq - nameval);
    if (n == 0) return -1;

    /* The name has to be nul-terminated on its own to be passed on.  Short ones
     * -- which is all of them in practice -- go on the stack; a long one is
     * borrowed from this allocator rather than truncated. */
    char small[128];
    char *name = small;
    if (n + 1 > sizeof(small)) {
        name = static_cast<char *>(util::host_alloc(n + 1));
        if (name == nullptr) return -1;
    }
    for (size_t i = 0; i < n; ++i) name[i] = nameval[i];
    name[n] = '\0';

    const int r = vesta_crt_putenv_s(name, eq + 1);
    if (name != small) util::host_free(name);
    return r == 0 ? 0 : -1;
}

/**
 * @brief `getenv_s`, which is the easy one: the caller brings the buffer.
 *
 * No lifetime to manage and nothing to remember, so it goes straight to the
 * block.  @p need comes back as the size INCLUDING the nul, or zero when the
 * variable is not set -- which is not an error, and says so by returning 0 with
 * @p need at zero.
 */
int vesta_crt_getenv_s(size_t *need, char *buf, size_t cap, const char *name) {
    if (need == nullptr || name == nullptr) return EINVAL;
    if (buf == nullptr && cap != 0) return EINVAL;
    *need = 0;

    const DWORD n = GetEnvironmentVariableA(name, nullptr, 0);
    if (n == 0) return 0; // not set: not an error
    *need = size_t(n);
    if (cap < size_t(n)) return ERANGE;

    const DWORD got = GetEnvironmentVariableA(name, buf, DWORD(cap));
    return (got != 0 && got < cap) ? 0 : EINVAL;
}

/* The wide side.  It is a separate list and not a template because this file is
 * read by people chasing a corruption, and a template that instantiates twice
 * over two character types is one more thing to hold in your head while doing
 * that.  See `EnvAnswer` for why anything is remembered at all. */
struct WEnvAnswer {
    WEnvAnswer *next;
    wchar_t *name;
    wchar_t *value;
    size_t value_cap;
};

std::atomic<WEnvAnswer *> g_wenv_head{nullptr};

bool wenv_name_equal(const wchar_t *a, const wchar_t *b) noexcept {
    for (;; ++a, ++b) {
        wchar_t x = *a, y = *b;
        if (x >= L'A' && x <= L'Z') x = wchar_t(x - L'A' + L'a');
        if (y >= L'A' && y <= L'Z') y = wchar_t(y - L'A' + L'a');
        if (x != y) return false;
        if (x == L'\0') return true;
    }
}

size_t wenv_len(const wchar_t *s) noexcept {
    size_t n = 0;
    while (s[n] != L'\0') ++n;
    return n;
}

/// @brief `_wgetenv`, the wide twin of @c vesta_crt_getenv.
wchar_t *vesta_crt_wgetenv(const wchar_t *name) {
    if (name == nullptr || name[0] == L'\0') return nullptr;

    const DWORD need = GetEnvironmentVariableW(name, nullptr, 0);
    if (need == 0) return nullptr;

    env_lock();
    WEnvAnswer *e = g_wenv_head.load(std::memory_order_relaxed);
    while (e != nullptr && !wenv_name_equal(e->name, name)) e = e->next;

    if (e == nullptr) {
        e = static_cast<WEnvAnswer *>(util::host_alloc(sizeof(WEnvAnswer)));
        if (e == nullptr) {
            env_unlock();
            return nullptr;
        }
        const size_t n = wenv_len(name);
        e->name =
            static_cast<wchar_t *>(util::host_alloc((n + 1) * sizeof(wchar_t)));
        if (e->name == nullptr) {
            util::host_free(e);
            env_unlock();
            return nullptr;
        }
        for (size_t i = 0; i <= n; ++i) e->name[i] = name[i];
        e->value = nullptr;
        e->value_cap = 0;
        e->next = g_wenv_head.load(std::memory_order_relaxed);
        g_wenv_head.store(e, std::memory_order_release);
    }

    if (e->value_cap < need) {
        wchar_t *bigger =
            static_cast<wchar_t *>(util::host_alloc(need * sizeof(wchar_t)));
        if (bigger == nullptr) {
            env_unlock();
            return nullptr;
        }
        e->value = bigger; // the old one is not released; see `EnvAnswer`
        e->value_cap = need;
    }
    const DWORD got =
        GetEnvironmentVariableW(name, e->value, DWORD(e->value_cap));
    env_unlock();
    return (got != 0 && got < e->value_cap) ? e->value : nullptr;
}

/// @brief `_wgetenv_s`, the wide twin of @c vesta_crt_getenv_s.
int vesta_crt_wgetenv_s(size_t *need, wchar_t *buf, size_t cap,
                        const wchar_t *name) {
    if (need == nullptr || name == nullptr) return EINVAL;
    if (buf == nullptr && cap != 0) return EINVAL;
    *need = 0;

    const DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return 0;
    *need = size_t(n);
    if (cap < size_t(n)) return ERANGE;

    const DWORD got = GetEnvironmentVariableW(name, buf, DWORD(cap));
    return (got != 0 && got < cap) ? 0 : EINVAL;
}

/// @brief `_wputenv_s`, the wide twin of @c vesta_crt_putenv_s.
int vesta_crt_wputenv_s(const wchar_t *name, const wchar_t *value) {
    if (name == nullptr || name[0] == L'\0') return EINVAL;
    for (const wchar_t *p = name; *p != L'\0'; ++p)
        if (*p == L'=') return EINVAL;
    const bool remove = value == nullptr || value[0] == L'\0';
    return SetEnvironmentVariableW(name, remove ? nullptr : value) ? 0 : EINVAL;
}

/// @brief `_wputenv`, the wide twin of @c vesta_crt_putenv.
int vesta_crt_wputenv(const wchar_t *nameval) {
    if (nameval == nullptr) return -1;
    const wchar_t *eq = nameval;
    while (*eq != L'\0' && *eq != L'=') ++eq;
    if (*eq != L'=') return -1;

    const size_t n = size_t(eq - nameval);
    if (n == 0) return -1;

    wchar_t small[128];
    wchar_t *name = small;
    if (n + 1 > sizeof(small) / sizeof(small[0])) {
        name =
            static_cast<wchar_t *>(util::host_alloc((n + 1) * sizeof(wchar_t)));
        if (name == nullptr) return -1;
    }
    for (size_t i = 0; i < n; ++i) name[i] = nameval[i];
    name[n] = L'\0';

    const int r = vesta_crt_wputenv_s(name, eq + 1);
    if (name != small) util::host_free(name);
    return r == 0 ? 0 : -1;
}

} // extern "C"

namespace util {

bool install_msvcrt_hook() noexcept {
    if (g_hooked.load(std::memory_order_acquire)) return true;

    /* NOTHING TO HOOK IF WE ARE NOT THE ALLOCATOR.  Every function below ends in
     * `host_alloc`, and when this library did not take over -- the region would
     * not reserve, or a static link did not pull the objects in -- that call has
     * nowhere to go: it refuses loudly rather than fall back in silence, which
     * is right everywhere except here, where the caller is the C runtime and a
     * refusal kills the process before `main`.
     *
     * That is not hypothetical: it is exactly how the old `VESTA_NO_HOST_SLAB`
     * failed, with the runtime asking for 105 bytes while starting up and dying
     * there.  The switch is gone, the way of failing is not.
     *
     * Asking here is safe this early: `host_alloc_active` decides on its first
     * call by reading the process environment block directly, and that
     * allocates nothing. */
    if (!host_alloc_active()) return false;

    HMODULE crt = GetModuleHandleA("msvcrt.dll");
    if (crt == nullptr) {
        std::fprintf(stderr,
                     "[allocator] msvcrt.dll is not loaded: what the C runtime "
                     "allocates inside itself stays outside this allocator.\n");
        return false;
    }

    /* PRIMERO, UNA PLAZA EN LA TABLA DEL RUNTIME -- y tiene que ser antes de
     * parchear, que es toda la gracia.
     *
     * Al desviar el registro a nuestra lista, alguien tiene que EJECUTARLA, y
     * el sitio importa: se probo en el desmontaje del proceso
     * (`DLL_PROCESS_DETACH`) y ahi el runtime ya ha cerrado los flujos, asi que
     * todo lo que quedaba por imprimir -- el informe de este mismo asignador --
     * se escribia en ninguna parte.  Corriendo desde una entrada de la tabla
     * original, la lista se ejecuta cuando le toca, con la salida todavia viva.
     *
     * Cabe UNA entrada, la nuestra, y se pide con la tabla aun intacta: si eso
     * la hace crecer, el `realloc` va al del runtime, que es quien sabe. */
    if (auto real_atexit = reinterpret_cast<int(__cdecl *)(ExitFn)>(
            GetProcAddress(crt, "atexit")))
        real_atexit(&run_exit_list);

    struct Entry {
        const char *name;
        const void *ours;
    };
    const Entry entries[16] = {
        {"malloc", reinterpret_cast<const void *>(&vesta_crt_malloc)},
        {"calloc", reinterpret_cast<const void *>(&vesta_crt_calloc)},
        {"realloc", reinterpret_cast<const void *>(&vesta_crt_realloc)},
        {"free", reinterpret_cast<const void *>(&vesta_crt_free)},
        /* \~english AND THE TWO THAT ONLY ASK QUESTIONS -- which is the whole
         * point of them being here.  Neither allocates anything, so neither
         * looks like it belongs in an allocator's hook list; both take a
         * pointer and put it to the NT heap, and that is what killed a window
         * inside `CreateWindowExW`.  Serving a block and letting somebody else
         * answer questions ABOUT it is being `malloc` by halves.  With these
         * two the family is closed: everything else in msvcrt that reaches the
         * heap reaches it through one of the six.
         *
         * \~spanish Y LAS DOS QUE SOLO PREGUNTAN -- que es justamente por lo
         * que estan aqui.  Ninguna reserva nada, asi que ninguna parece de la
         * lista de ganchos de un asignador; las dos cogen un puntero y se lo
         * plantean al monton NT, y eso es lo que mato una ventana dentro de
         * `CreateWindowExW`.  Servir un bloque y dejar que otro conteste
         * preguntas SOBRE el es ser `malloc` a medias.  Con estas dos la
         * familia queda cerrada: todo lo demas de msvcrt que llega al monton
         * llega por una de las seis.  \~ */
        {"_msize", reinterpret_cast<const void *>(&vesta_crt_msize)},
        {"_expand", reinterpret_cast<const void *>(&vesta_crt_expand)},
        /* Y EL REGISTRO DE SALIDA, que no reserva pero es quien HACIA reservar:
         * su tabla crece con `realloc`, nacio antes del parche, y era el unico
         * bloque ajeno que llegaba a un camino caliente.  Ver la lista propia,
         * arriba. */
        {"atexit", reinterpret_cast<const void *>(&vesta_crt_atexit)},
        {"_onexit", reinterpret_cast<const void *>(&vesta_crt_onexit)},
        /* Y EL ENTORNO, LA FAMILIA ENTERA.  Las ocho, y no las dos que fallaban:
         * con la mitad puesta, escribir por una puerta y leer por la otra da una
         * respuesta desfasada en vez de un error -- que es el mismo modo de
         * fallo que se esta quitando, con otro disfraz.  Ver el bloque de arriba
         * sobre por que la verdad es el bloque del proceso. */
        {"getenv", reinterpret_cast<const void *>(&vesta_crt_getenv)},
        {"getenv_s", reinterpret_cast<const void *>(&vesta_crt_getenv_s)},
        {"_putenv", reinterpret_cast<const void *>(&vesta_crt_putenv)},
        {"_putenv_s", reinterpret_cast<const void *>(&vesta_crt_putenv_s)},
        {"_wgetenv", reinterpret_cast<const void *>(&vesta_crt_wgetenv)},
        {"_wgetenv_s", reinterpret_cast<const void *>(&vesta_crt_wgetenv_s)},
        {"_wputenv", reinterpret_cast<const void *>(&vesta_crt_wputenv)},
        {"_wputenv_s", reinterpret_cast<const void *>(&vesta_crt_wputenv_s)}};

    bool all_ok = true;
    for (const Entry &e : entries) {
        void *entry = reinterpret_cast<void *>(GetProcAddress(crt, e.name));
        if (entry == nullptr || !write_jump_far(entry, e.ours)) {
            std::fprintf(stderr,
                         "[allocator] could not redirect msvcrt!%s: its "
                         "allocations will not appear in the report.\n",
                         e.name);
            all_ok = false;
        }
    }
    g_hooked.store(all_ok, std::memory_order_release);
    return all_ok;
}

bool msvcrt_hook_installed() noexcept {
    return g_hooked.load(std::memory_order_acquire);
}

unsigned long long msvcrt_stranded_blocks() noexcept {
    return g_foreign.load(std::memory_order_relaxed);
}

} // namespace util

// ===========================================================================
//  El disparador: un callback de TLS, que corre antes que nada del proceso
// ===========================================================================

extern "C" {

/**
 * @brief Pone los saltos, antes del punto de entrada del proceso.
 *
 * UN CALLBACK DE TLS Y NO UN CONSTRUCTOR, y la diferencia se mide: cuando corre
 * un constructor, el runtime de C lleva ya trece bloques reservados; aqui lleva
 * nueve.  Los cuatro de diferencia incluyen la tabla de `atexit`, que
 * `_onexit` hace CRECER con `realloc` -- parcheando tarde, ese `realloc`
 * llegaba con un bloque ajeno y no habia forma honesta de servirlo.
 *
 * Se ejecuta con un solo hilo y antes de que nadie mas pueda estar dentro de
 * las funciones que se parchean, que es justo lo que escribir sobre codigo en
 * ejecucion necesita.
 */
void NTAPI vesta_msvcrt_tls_callback(PVOID, DWORD reason, PVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        util::install_msvcrt_hook();
        return;
    }
    /* Y AL SALIR, lo que se registro en nuestra lista.  Tiene que correrlo
     * alguien: interceptar el registro sin ejecutar lo registrado seria peor
     * que no interceptarlo -- los destructores estaticos, el informe del propio
     * asignador, todo lo que un programa deja para el final, dejaria de
     * ejecutarse y nadie lo diria.
     *
     * Aqui, en el desmontaje del proceso, que es el ultimo sitio al que esta
     * libreria llega y no depende de que el runtime siga en pie. */
    if (reason == DLL_PROCESS_DETACH) run_exit_list();
}

/**
 * @brief El ancla que arrastra este objeto al enlace.
 *
 * Esta libreria es un archivo estatico, y de un archivo solo se saca el objeto
 * que resuelva algun simbolo PENDIENTE.  Nadie llama a lo de aqui -- ese es el
 * punto: el gancho se pone solo --, asi que sin una referencia forzada el
 * objeto no entraria y no pasaria absolutamente nada, en silencio.  El CMake
 * pide este simbolo con `-Wl,-u`.
 */
void vesta_msvcrt_hook_anchor(void) {}

} // extern "C"

/**
 * @brief La entrada en el directorio de TLS.
 *
 * `.CRT$XLB` es donde el enlazador junta los callbacks de TLS; `used` es
 * obligatorio porque nadie referencia esta variable y el barrido de secciones
 * (`--gc-sections`, que este proyecto usa) se la llevaria.
 */
extern "C" const PIMAGE_TLS_CALLBACK vesta_msvcrt_tls_entry
    __attribute__((section(".CRT$XLB"), used)) = vesta_msvcrt_tls_callback;

#endif // _WIN32

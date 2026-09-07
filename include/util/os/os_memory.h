/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/os/os_memory.h
 * @brief
 * \~english Asking the SYSTEM for memory, keeping booking apart from handing
 *          over.
 * \~spanish Pedir memoria AL SISTEMA, separando apalabrar de entregar.
 * \~
 *
 * \~english
 * WHY IT EXISTS, WHEN THERE IS ALREADY `vm::allocate_memory`.  Because that one
 * is no good for whoever is BELOW the allocators, for three independent
 * reasons:
 *
 *  1. **It always commits** (`MEM_COMMIT | MEM_RESERVE`).  The allocator needs
 *     precisely the opposite: to book a large range of ADDRESSES without
 *     spending memory, and hand it over afterwards chunk by chunk.  Out of that
 *     separation comes the property that makes freeing cheap -- telling whether
 *     a pointer is ours is two compares -- so it is not a detail to give away.
 *  2. **On failure, it ALLOCATES memory**: it writes through `VGC_CERR` and
 *     uses `FormatMessageA` with `FORMAT_MESSAGE_ALLOCATE_BUFFER`.  Both of
 *     those ask for memory, and asking for memory re-enters the allocator.
 *     Whoever resolves allocations cannot lean on anything that allocates.
 *  3. **It drags `windows.h`** into anybody who includes it, and that header
 *     defines `VOID` as a macro and breaks every `enum class` using that name.
 *
 * WHAT THIS IS.  The only place in the project that talks to the operating
 * system about memory below the allocators.  Five functions, with no state,
 * allocating nothing, printing nothing: a failure is a return value.  The
 * header includes NEITHER `windows.h` NOR `sys/mman.h`; that lives in the
 * `.cpp`.
 *
 * WHO USES IT.  The allocator (its region), the bump arena, the parallel
 * metadata of the diagnostic mode and the large-allocation path.  All of them
 * need the same thing, and having it in one place is what stops each of them
 * writing its own `VirtualAlloc`.
 *
 * IT DOES NOT REPLACE `vm::allocate_memory`, which still serves the virtual
 * machine's arenas, with permissions and with diagnostics.  What that one wants
 * whenever it is touched is to LEAN on this instead of repeating the dealings
 * with the system.
 *
 * \~spanish
 * POR QUE EXISTE, SI YA HAY `vm::allocate_memory`.  Porque esa no vale para
 * quien esta POR DEBAJO de las reservas, y por tres motivos independientes:
 *
 *  1. **Compromete siempre** (`MEM_COMMIT | MEM_RESERVE`).  El asignador
 *     necesita justo lo contrario: apalabrar un rango grande de DIRECCIONES sin
 *     gastar memoria, y entregarla despues trozo a trozo.  De esa separacion
 *     sale la propiedad que hace barato liberar -- saber si un puntero es
 *     nuestro son dos comparaciones --, asi que no es un detalle cedible.
 *  2. **Al fallar, RESERVA memoria**: escribe por `VGC_CERR` y usa
 *     `FormatMessageA` con `FORMAT_MESSAGE_ALLOCATE_BUFFER`.  Las dos cosas
 *     piden memoria, y pedir memoria vuelve a entrar en el asignador.  Quien
 *     resuelve las reservas no puede apoyarse en nada que reserve.
 *  3. **Arrastra `windows.h`** hasta cualquiera que la incluya, y esa cabecera
 *     define `VOID` como macro y rompe todo `enum class` que use ese nombre.
 *
 * QUE ES ESTO.  El unico sitio del proyecto que habla de memoria con el sistema
 * operativo por debajo de las reservas.  Cinco funciones, sin estado, sin
 * reservar nada, sin imprimir nada: un fallo es un valor de retorno.  La
 * cabecera no incluye NI `windows.h` NI `sys/mman.h`; eso vive en el `.cpp`.
 *
 * QUIEN LA USA.  El asignador (su region), la arena de golpe, los metadatos en
 * paralelo del modo de diagnostico y el camino de reservas grandes.  Todos
 * necesitan lo mismo, y tenerlo en un sitio es lo que evita que cada uno
 * escriba su propio `VirtualAlloc`.
 *
 * NO SUSTITUYE a `vm::allocate_memory`, que sigue sirviendo a las arenas de la
 * maquina virtual, con permisos y con diagnostico.  Lo suyo, cuando se toque,
 * es que aquella se APOYE en esta en vez de repetir el trato con el sistema.
 *
 * \~
 */
#ifndef VESTA_UTIL_OS_MEMORY_H
#define VESTA_UTIL_OS_MEMORY_H

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace util {

namespace detail {

/**
 * @brief
 * \~english What the system says about its sizes, asked ONCE only.
 * \~spanish Lo que dice el sistema de sus tamanos, preguntado UNA sola vez.
 * \~
 *
 * \~english
 * Declared here, and not hidden in the `.cpp`, so that consulting them is a
 * read and not a call.  `vm::allocate_memory` rounds to pages on EVERY
 * allocation: it used to do a `GetSystemInfo` (forty bytes of structure on the
 * stack plus a query to the system) to work out a number that never changes,
 * and that alone was enough for the function not to fit inside its caller.
 *
 * Zero means "not asked yet".  They are not API: they are read through
 * @c os_page_size and @c os_reserve_granularity.
 *
 * \~spanish
 * Declarados aqui, y no escondidos en el `.cpp`, para que consultarlos sea una
 * lectura y no una llamada.  `vm::allocate_memory` redondea a paginas en CADA
 * reserva: antes hacia un `GetSystemInfo` (cuarenta bytes de estructura en la
 * pila mas una consulta al sistema) para averiguar un numero que no cambia
 * nunca, y eso solo bastaba para que la funcion no cupiera dentro de quien la
 * llamaba.
 *
 * Cero significa "todavia no se ha preguntado".  No son API: se leen por
 * @c os_page_size y @c os_reserve_granularity.
 *
 * \~
 */
extern std::atomic<size_t> g_page_size;
/// \~english The alignment at which the system hands out booked ranges.
/// \~spanish La alineacion con la que el sistema entrega rangos apalabrados.
/// \~
extern std::atomic<size_t> g_granularity;

/**
 * @brief
 * \~english Asks the system and fills both in.  Out of line: it happens once.
 * \~spanish Pregunta al sistema y rellena los dos.  Fuera de linea: pasa una
 *          vez.
 * \~
 */
void query_os_sizes() noexcept;

} // namespace detail

/**
 * @brief
 * \~english The permissions of a memory region.
 * \~spanish Permisos de una region de memoria.
 * \~
 *
 * \~english
 * Separate bits so that they can be combined with `|`.  They are translated
 * into whatever the system understands INSIDE the `.cpp`; the caller does not
 * have to know whether this ends up as a `PAGE_EXECUTE_READWRITE` or as a
 * `PROT_READ | PROT_EXEC`, which is exactly what lets this header include
 * nothing of the system's.
 *
 * \~spanish
 * Bits sueltos para poder combinarlos con `|`.  Se traducen a lo que entienda
 * el sistema DENTRO del `.cpp`; quien llama no tiene que saber si esto acaba en
 * un `PAGE_EXECUTE_READWRITE` o en un `PROT_READ | PROT_EXEC`, que es
 * justamente lo que permite que esta cabecera no incluya nada del sistema.
 *
 * \~
 */
enum class OsProt : unsigned {
    /// \~english no access at all  \~spanish ningun acceso  \~
    None = 0,
    /// \~english readable  \~spanish se puede leer  \~
    Read = 1u << 0,
    /// \~english writable  \~spanish se puede escribir  \~
    Write = 1u << 1,
    /// \~english pages of generated code (JIT)
    /// \~spanish paginas de codigo generado (JIT)  \~
    Exec = 1u << 2,
};

/**
 * @brief
 * \~english Combines two permission sets.
 * \~spanish Combina dos juegos de permisos.
 * \~
 * @param a
 * \~english one of them.
 * \~spanish uno de ellos.
 * \~
 * @param b
 * \~english the other.
 * \~spanish el otro.
 * \~
 * @return
 * \~english the union of both.
 * \~spanish la union de los dos.
 * \~
 */
constexpr OsProt operator|(OsProt a, OsProt b) {
    return static_cast<OsProt>(static_cast<unsigned>(a) |
                               static_cast<unsigned>(b));
}

/**
 * @brief
 * \~english Whether a permission set carries one bit.
 * \~spanish Si un juego de permisos lleva un bit.
 * \~
 * @param set
 * \~english the set to look at.
 * \~spanish el juego que hay que mirar.
 * \~
 * @param bit
 * \~english the permission being asked about.
 * \~spanish el permiso por el que se pregunta.
 * \~
 * @return
 * \~english true when the bit is in the set.
 * \~spanish true cuando el bit esta en el juego.
 * \~
 */
constexpr bool has_prot(OsProt set, OsProt bit) {
    return (static_cast<unsigned>(set) & static_cast<unsigned>(bit)) != 0;
}

/// \~english What is asked for almost always: read and write.
/// \~spanish Lo que se pide casi siempre: leer y escribir.
/// \~
inline constexpr OsProt kOsReadWrite = OsProt::Read | OsProt::Write;

/**
 * @brief
 * \~english Books and HANDS OVER in one go, with the permissions asked for.
 * \~spanish Apalabra y ENTREGA de una vez, con los permisos pedidos.
 * \~
 *
 * \~english
 * It is what whoever just needs a block and nothing more wants: booking and
 * handing over separately only pays when it is going to be handed over IN
 * PARTS.
 *
 * @par Threads
 * Safe from any thread.
 *
 * \~spanish
 * Es lo que quiere quien solo necesita un bloque y ya: apalabrar y entregar
 * por separado solo compensa cuando se va a entregar POR PARTES.
 *
 * @par Hilos
 * Segura desde cualquier hilo.
 *
 * \~
 * @param bytes
 * \~english how much is wanted; it is rounded up to whole pages.
 * \~spanish cuanto se quiere; se redondea a paginas hacia arriba.
 * \~
 * @param prot
 * \~english the permissions the block is handed over with.
 * \~spanish los permisos con los que se entrega el bloque.
 * \~
 * @return
 * \~english the base of the block, or nullptr if the system cannot.
 * \~spanish la base del bloque, o nullptr si el sistema no puede.
 * \~
 *
 * \~english
 * @code
 *   // One page of generated code.
 *   void *code = util::os_alloc(4096, util::OsProt::Read |
 *                                     util::OsProt::Write |
 *                                     util::OsProt::Exec);
 *   if (code == nullptr) return false;
 *   ...
 *   util::os_free(code, 4096);
 * @endcode
 *
 * \~spanish
 * @code
 *   // Una pagina de codigo generado.
 *   void *code = util::os_alloc(4096, util::OsProt::Read |
 *                                     util::OsProt::Write |
 *                                     util::OsProt::Exec);
 *   if (code == nullptr) return false;
 *   ...
 *   util::os_free(code, 4096);
 * @endcode
 *
 * \~
 */
void *os_alloc(size_t bytes, OsProt prot) noexcept;

/**
 * @brief
 * \~english Releases a block from @c os_alloc.
 * \~spanish Suelta un bloque de @c os_alloc.
 * \~
 *
 * \~english
 * @par Threads
 * Safe.
 *
 * \~spanish
 * @par Hilos
 * Segura.
 *
 * \~
 * @param addr
 * \~english the base @c os_alloc returned.
 * \~spanish la base que devolvio @c os_alloc.
 * \~
 * @param bytes
 * \~english the same size that was asked for, before rounding.
 * \~spanish el mismo tamano que se pidio, antes de redondear.
 * \~
 */
void os_free(void *addr, size_t bytes) noexcept;

/**
 * @brief
 * \~english Changes the permissions of a stretch already handed over.
 * \~spanish Cambia los permisos de un tramo ya entregado.
 * \~
 *
 * \~english
 * It is used by whoever writes code and then wants to run it: it is handed over
 * writable, filled in, and turned into executable.  Having both at once works,
 * but it leaves pages writable AND executable, which is what no modern
 * operating system wants to see.
 *
 * @par Threads
 * Safe as long as the stretches do not overlap.
 *
 * \~spanish
 * Lo usa quien escribe codigo y luego quiere ejecutarlo: se entrega con
 * escritura, se rellena, y se pasa a ejecucion.  Tener las dos cosas a la vez
 * funciona, pero deja paginas escribibles Y ejecutables, que es lo que ningun
 * sistema operativo moderno quiere ver.
 *
 * @par Hilos
 * Segura mientras los tramos no se solapen.
 *
 * \~
 * @param addr
 * \~english the start of the stretch.
 * \~spanish el principio del tramo.
 * \~
 * @param bytes
 * \~english how much of it changes.
 * \~spanish cuanto de el cambia.
 * \~
 * @param prot
 * \~english the new permissions.
 * \~spanish los permisos nuevos.
 * \~
 * @return
 * \~english false if the system does not allow it.
 * \~spanish false si el sistema no lo permite.
 * \~
 *
 * \~english
 * @code
 *   vesta_memcpy(code, bytes, n);                      // write
 *   util::os_protect(code, n, util::OsProt::Read |
 *                             util::OsProt::Exec);     // and now only run
 * @endcode
 *
 * \~spanish
 * @code
 *   vesta_memcpy(code, bytes, n);                      // escribir
 *   util::os_protect(code, n, util::OsProt::Read |
 *                             util::OsProt::Exec);     // y ya solo ejecutar
 * @endcode
 *
 * \~
 */
bool os_protect(void *addr, size_t bytes, OsProt prot) noexcept;

/**
 * @brief
 * \~english Books @p bytes of ADDRESSES without spending memory.
 * \~spanish Apalabra @p bytes de DIRECCIONES sin gastar memoria.
 * \~
 *
 * \~english
 * What comes back can NOT be read or written until it goes through
 * @c os_commit.  Booking consumes address space, of which there is plenty on 64
 * bits, but **it is not entirely free**: the system notes down the page
 * bookkeeping, and that works out at some 2.7 MiB and half a millisecond per
 * TiB booked.
 *
 * @par Threads
 * Safe from any thread.  Every call returns a different range.
 *
 * \~spanish
 * Lo devuelto NO se puede leer ni escribir hasta pasarlo por @c os_commit.
 * Apalabrar consume espacio de direcciones, que en 64 bits sobra, pero **no es
 * gratis del todo**: el sistema apunta la contabilidad de paginas, y sale a
 * unos 2,7 MiB y medio milisegundo por TiB apalabrado.
 *
 * @par Hilos
 * Segura desde cualquier hilo.  Cada llamada devuelve un rango distinto.
 *
 * \~
 * @param bytes
 * \~english how many addresses to book.
 * \~spanish cuantas direcciones apalabrar.
 * \~
 * @return
 * \~english the base of the range, or nullptr if the system cannot.
 * \~spanish la base del rango, o nullptr si el sistema no puede.
 * \~
 *
 * \~english
 * @code
 *   // Book 1 GiB and use only the first chunk.
 *   void *base = util::os_reserve(size_t(1) << 30);
 *   if (base == nullptr) return false;              // the system said no
 *   if (!util::os_commit(base, 64 * 1024)) {        // hand over 64 KiB
 *       util::os_release(base, size_t(1) << 30);
 *       return false;
 *   }
 *   vesta_memset(base, 0, 64 * 1024);               // now it can be written
 * @endcode
 *
 * \~spanish
 * @code
 *   // Apalabrar 1 GiB y usar solo el primer trozo.
 *   void *base = util::os_reserve(size_t(1) << 30);
 *   if (base == nullptr) return false;              // el sistema dijo que no
 *   if (!util::os_commit(base, 64 * 1024)) {        // entregar 64 KiB
 *       util::os_release(base, size_t(1) << 30);
 *       return false;
 *   }
 *   vesta_memset(base, 0, 64 * 1024);              // ya se puede escribir
 * @endcode
 *
 * \~
 */
void *os_reserve(size_t bytes) noexcept;

/**
 * @brief
 * \~english Hands over real memory in a stretch already booked, read/write.
 * \~spanish Entrega memoria de verdad en un tramo ya apalabrado,
 *          lectura/escritura.
 * \~
 *
 * \~english
 * Handing over the same stretch twice is not an error: the second time does
 * nothing.
 *
 * @par Threads
 * Safe from any thread **as long as the stretches do not overlap**.  Two
 * threads handing over the SAME stretch at once is correct on Windows and on
 * POSIX, but whoever hands out the stretches should already be preventing it.
 *
 * \~spanish
 * Entregar dos veces el mismo tramo no es un error: la segunda no hace nada.
 *
 * @par Hilos
 * Segura desde cualquier hilo **siempre que los tramos no se solapen**.  Dos
 * hilos entregando el MISMO tramo a la vez es correcto en Windows y en POSIX,
 * pero quien reparta los tramos ya deberia estar impidiendolo.
 *
 * \~
 * @param addr
 * \~english inside a range returned by @c os_reserve.
 * \~spanish dentro de un rango devuelto por @c os_reserve.
 * \~
 * @param bytes
 * \~english the size of the stretch; rounded up to whole pages.
 * \~spanish tamano del tramo; se redondea a paginas hacia arriba.
 * \~
 * @param prot
 * \~english the permissions it is handed over with; read and write by default.
 * \~spanish los permisos con los que se entrega; leer y escribir por defecto.
 * \~
 * @return
 * \~english false if the system cannot give more memory.
 * \~spanish false si el sistema no puede dar mas memoria.
 * \~
 *
 * \~english
 * @code
 *   // Grow one chunk at a time, as it is needed.
 *   char *page = static_cast<char *>(base) + used;
 *   if (!util::os_commit(page, 64 * 1024)) return nullptr;  // out of memory
 *   used += 64 * 1024;
 * @endcode
 *
 * \~spanish
 * @code
 *   // Crecer un trozo cada vez, segun hace falta.
 *   char *page = static_cast<char *>(base) + usados;
 *   if (!util::os_commit(page, 64 * 1024)) return nullptr;  // sin memoria
 *   usados += 64 * 1024;
 * @endcode
 *
 * \~
 */
bool os_commit(void *addr, size_t bytes, OsProt prot = kOsReadWrite) noexcept;

/**
 * @brief
 * \~english Gives a stretch's memory back to the system, KEEPING its address.
 * \~spanish Devuelve la memoria de un tramo al sistema, CONSERVANDO su
 *          direccion.
 * \~
 *
 * \~english
 * It is what allows the peak to be brought down without losing the range: the
 * stretch is still ours and can be handed over again with @c os_commit.
 * **Whatever was inside is lost**, and reading it again without handing it over
 * fails -- which is what is wanted: a use after release has to give a loud
 * failure, not zeroes in silence.
 *
 * @par Threads
 * Safe, with the same condition as @c os_commit: stretches that do not overlap.
 *
 * \~spanish
 * Es lo que permite bajar el pico sin perder el rango: el tramo sigue siendo
 * nuestro y se puede volver a entregar con @c os_commit.  **Lo que hubiera
 * dentro se pierde**, y volver a leerlo sin entregarlo otra vez falla -- que es
 * lo que se quiere: un uso despues de soltar tiene que dar un fallo ruidoso, no
 * ceros en silencio.
 *
 * @par Hilos
 * Segura, con la misma condicion que @c os_commit: tramos que no se solapen.
 *
 * \~
 * @param addr
 * \~english the start of the stretch.
 * \~spanish el principio del tramo.
 * \~
 * @param bytes
 * \~english how much of it is given back.
 * \~spanish cuanto de el se devuelve.
 * \~
 * @return
 * \~english false if the system did not accept it; the stretch then keeps its
 *           memory.
 * \~spanish false si el sistema no lo acepto; el tramo se queda entonces con su
 *           memoria.
 * \~
 *
 * \~english
 * @code
 *   // A stretch that ran empty: its pages are released and the place is kept
 *   // for reuse without asking the system for addresses again.
 *   util::os_decommit(stretch, bytes);
 *   keep_for_reuse(stretch, bytes);
 *   // ...later on
 *   util::os_commit(stretch, bytes);   // it has memory behind it again
 * @endcode
 *
 * \~spanish
 * @code
 *   // Un tramo que se quedo vacio: se sueltan sus paginas y se guarda el sitio
 *   // para reusarlo sin volver a pedirle direcciones al sistema.
 *   util::os_decommit(tramo, bytes);
 *   guardar_para_reusar(tramo, bytes);
 *   // ...mas tarde
 *   util::os_commit(tramo, bytes);   // vuelve a tener memoria detras
 * @endcode
 *
 * \~
 */
bool os_decommit(void *addr, size_t bytes) noexcept;

/**
 * @brief
 * \~english Releases a booked range WHOLE.
 * \~spanish Suelta un rango apalabrado ENTERO.
 * \~
 *
 * \~english
 * @par Threads
 * Safe, but releasing a range another thread is using is the caller's failure,
 * not this one's.
 *
 * \~spanish
 * @par Hilos
 * Segura, pero soltar un rango que otro hilo este usando es un fallo del que
 * llama, no de aqui.
 *
 * \~
 * @param addr
 * \~english exactly what @c os_reserve returned.
 * \~spanish exactamente lo que devolvio @c os_reserve.
 * \~
 * @param bytes
 * \~english the same size it was asked for.
 * \~spanish el mismo tamano que se le pidio.
 * \~
 *
 * \~english
 * @code
 *   util::os_release(base, size_t(1) << 30);   // the range stops being ours
 * @endcode
 *
 * \~spanish
 * @code
 *   util::os_release(base, size_t(1) << 30);   // el rango deja de ser nuestro
 * @endcode
 *
 * \~
 */
void os_release(void *addr, size_t bytes) noexcept;

/**
 * @brief
 * \~english The system's page size.  Asked once and remembered.
 * \~spanish Tamano de pagina del sistema.  Se consulta una vez y se recuerda.
 * \~
 *
 * \~english
 * @par Threads
 * Safe.  Two threads arriving first both ask and both write the same thing,
 * which is not a problem; there is no guard variable involved (those already
 * cost a hang, see `util/os/thread_slot.h`).
 *
 * \~spanish
 * @par Hilos
 * Segura.  Dos hilos que lleguen los primeros preguntan los dos y escriben lo
 * mismo, que no es un problema; no hay variable de guarda de por medio (esas ya
 * costaron un cuelgue, ver `util/os/thread_slot.h`).
 *
 * \~
 * @return
 * \~english the page size in bytes, always a power of two.
 * \~spanish el tamano de pagina en bytes, siempre potencia de dos.
 * \~
 *
 * \~english
 * @code
 *   const size_t page = util::os_page_size();
 *   const size_t rounded = (n + page - 1) & ~(page - 1);
 * @endcode
 *
 * \~spanish
 * @code
 *   const size_t page = util::os_page_size();
 *   const size_t redondeado = (n + page - 1) & ~(page - 1);
 * @endcode
 *
 * \~
 */
[[gnu::always_inline]] inline size_t os_page_size() noexcept {
    const size_t v = detail::g_page_size.load(std::memory_order_relaxed);
    if (v != 0) return v;
    detail::query_os_sizes();
    return detail::g_page_size.load(std::memory_order_relaxed);
}

/**
 * @brief
 * \~english At what alignment @c os_reserve hands out addresses.
 * \~spanish Con que alineacion entrega direcciones @c os_reserve.
 * \~
 *
 * \~english
 * On Windows it is 64 KiB, not the page size, and there is code that depends on
 * it -- the allocator locates a pointer's chunk by masking -- so it is asked
 * rather than assumed.
 *
 * @par Threads
 * Safe, the same as @c os_page_size.
 *
 * \~spanish
 * En Windows son 64 KiB, no el tamano de pagina, y hay codigo que depende de
 * ello -- el asignador localiza el trozo de un puntero enmascarando --, asi que
 * se pregunta en vez de suponerse.
 *
 * @par Hilos
 * Segura, igual que @c os_page_size.
 *
 * \~
 * @return
 * \~english the granularity in bytes, always a power of two.
 * \~spanish la granularidad en bytes, siempre potencia de dos.
 * \~
 *
 * \~english
 * @code
 *   // Align the base to what the system really hands out, not to a guess.
 *   const uintptr_t g = util::os_reserve_granularity();
 *   const uintptr_t base = (raw + g - 1) & ~(g - 1);
 * @endcode
 *
 * \~spanish
 * @code
 *   // Alinear la base a lo que de verdad entrega el sistema, no a lo supuesto.
 *   const uintptr_t g = util::os_reserve_granularity();
 *   const uintptr_t base = (bruta + g - 1) & ~(g - 1);
 * @endcode
 *
 * \~
 */
[[gnu::always_inline]] inline size_t os_reserve_granularity() noexcept {
    const size_t v = detail::g_granularity.load(std::memory_order_relaxed);
    if (v != 0) return v;
    detail::query_os_sizes();
    return detail::g_granularity.load(std::memory_order_relaxed);
}

/**
 * @brief
 * \~english Books the largest range the system grants between two limits.
 * \~spanish Apalabra el mayor rango que el sistema conceda entre dos limites.
 * \~
 *
 * \~english
 * WHY IT EXISTS.  Because how much can be booked **is not known until it is
 * asked**, and it depends on the machine, on the system and on what the process
 * has already done.  Writing a constant is assuming, and assuming here degrades
 * quietly: whoever asks for too much gets nothing and falls back to the system
 * allocator without anybody finding out.  It goes on halving until it fits, so
 * on a tight machine LESS is obtained but something is.
 *
 * @par Threads
 * Safe from any thread.
 *
 * \~spanish
 * POR QUE EXISTE.  Porque cuanto se puede apalabrar **no se sabe hasta que se
 * pregunta**, y depende de la maquina, del sistema y de lo que ya haya hecho el
 * proceso.  Escribir una constante es suponer, y suponer aqui degrada en
 * silencio: quien pide de mas se queda sin nada y cae al asignador del sistema
 * sin que se entere nadie.  Va bajando a la mitad hasta que entra, asi que en
 * una maquina apretada se consigue MENOS pero se consigue.
 *
 * @par Hilos
 * Segura desde cualquier hilo.
 *
 * \~
 * @param max
 * \~english what one would like.
 * \~spanish lo que se querria.
 * \~
 * @param min
 * \~english below this it is not worth it; it gives up and returns nullptr.
 * \~spanish por debajo de esto no merece la pena; se rinde y da nullptr.
 * \~
 * @param got
 * \~english out: how much was really obtained.  Zero when nothing could be.
 * \~spanish sale: cuanto se consiguio de verdad.  Cero si no se pudo nada.
 * \~
 * @return
 * \~english the base of the range, or nullptr.
 * \~spanish la base del rango, o nullptr.
 * \~
 *
 * \~english
 * @code
 *   // Ask for 256 GiB, settle for 64 MiB, and KNOW what one ended up with.
 *   size_t got = 0;
 *   void *base = util::os_reserve_largest(size_t(256) << 30,
 *                                         size_t(64) << 20, &got);
 *   if (base == nullptr) return false;      // not even the minimum: no region
 *   // `got` is what is really there; use that, not the maximum asked for.
 * @endcode
 *
 * \~spanish
 * @code
 *   // Pedir 256 GiB, conformarse con 64 MiB, y SABER con cuanto se quedo uno.
 *   size_t conseguido = 0;
 *   void *base = util::os_reserve_largest(size_t(256) << 30,
 *                                         size_t(64) << 20, &conseguido);
 *   if (base == nullptr) return false;      // ni el minimo: no hay region
 *   // `conseguido` es lo que hay de verdad; usarlo, no el maximo que se pidio.
 * @endcode
 *
 * \~
 */
void *os_reserve_largest(size_t max, size_t min, size_t *got) noexcept;

/**
 * @brief
 * \~english What the system says about THIS process's memory, right now.
 * \~spanish Lo que el sistema dice de la memoria de ESTE proceso, ahora mismo.
 * \~
 *
 * \~english
 * It is a DIFFERENT question from the one `util::host_alloc_stats()` answers,
 * and it is worth not confusing them: that one says what OUR allocator asked
 * for, this one what the operating system has noted down for the whole process
 * -- including what third-party libraries allocated, the executable's own code
 * and the threads' stacks.
 *
 * All in bytes.  A field at zero means this system cannot say.
 *
 * \~spanish
 * Es una pregunta DISTINTA de la que contesta `util::host_alloc_stats()`, y
 * conviene no confundirlas: aquella dice lo que pidio NUESTRO asignador, esta
 * lo que el sistema operativo tiene apuntado del proceso entero -- incluido lo
 * que reservaron las librerias de terceros, el codigo del propio ejecutable y
 * las pilas de los hilos.
 *
 * Todo en bytes.  Un campo a cero significa que este sistema no lo sabe decir.
 *
 * \~
 */
struct OsProcessMemory {
    /// \~english resident in RAM right now
    /// \~spanish residente en RAM ahora mismo  \~
    uint64_t working_set = 0;
    /// \~english the most that ever was resident
    /// \~spanish lo mayor que llego a estar residente  \~
    uint64_t working_set_peak = 0;
    /// \~english private memory committed
    /// \~spanish memoria privada comprometida  \~
    uint64_t commit = 0;
    /// \~english the largest value it reached
    /// \~spanish el mayor valor que alcanzo  \~
    uint64_t commit_peak = 0;
};

/**
 * @brief
 * \~english Asks the system about this process.
 * \~spanish Pregunta al sistema por este proceso.
 * \~
 *
 * \~english
 * It allocates no memory, so it can be called from a tight spot -- right when
 * the system has just said no.  It is not meant for a hot path: inside, it
 * queries the system.
 *
 * @par Threads
 * Safe from any thread.  It returns a copy, not a reference to state.
 *
 * \~spanish
 * No reserva memoria, asi que se puede llamar desde un sitio apretado -- justo
 * cuando el sistema acaba de decir que no --.  No esta pensada para un camino
 * caliente: por dentro consulta al sistema.
 *
 * @par Hilos
 * Segura desde cualquier hilo.  Devuelve una copia, no una referencia a estado.
 *
 * \~
 * @return
 * \~english the figures, with a zero in whatever this system does not report.
 * \~spanish las cifras, con un cero en lo que este sistema no diga.
 * \~
 *
 * \~english
 * @code
 *   // How much THIS process came to take, which is the question the
 *   // allocator's counters do NOT answer: third-party memory counts here too.
 *   const auto m = util::os_process_memory();
 *   std::printf("peak %.1f MiB\n", m.working_set_peak / (1024.0 * 1024.0));
 * @endcode
 *
 * \~spanish
 * @code
 *   // Cuanto llego a ocupar ESTE proceso, que es la pregunta que los
 *   // contadores del asignador NO contestan: ahi entra tambien lo de terceros.
 *   const auto m = util::os_process_memory();
 *   std::printf("pico %.1f MiB\n", m.working_set_peak / (1024.0 * 1024.0));
 * @endcode
 *
 * \~
 */
OsProcessMemory os_process_memory() noexcept;

/**
 * @brief
 * \~english What the system says about ITSELF.
 * \~spanish Lo que el sistema dice de SI MISMO.
 * \~
 *
 * \~english
 * `physical_available` and `address_space_free` DO change, so they are asked
 * for every time; the other two do not, but they come along on the same trip.
 *
 * \~spanish
 * `physical_available` y `address_space_free` CAMBIAN, asi que se preguntan
 * cada vez; los otros dos no, pero salen en el mismo viaje.
 *
 * \~
 */
struct OsSystemMemory {
    /// \~english RAM installed  \~spanish RAM instalada  \~
    uint64_t physical_total = 0;
    /// \~english RAM free now  \~spanish RAM libre ahora  \~
    uint64_t physical_available = 0;
    /// \~english how much the process can address
    /// \~spanish cuanto puede direccionar el proceso  \~
    uint64_t address_space_total = 0;
    /// \~english how much is left unbooked
    /// \~spanish cuanto queda sin apalabrar  \~
    uint64_t address_space_free = 0;
};

/**
 * @brief
 * \~english Asks the system about itself.  The same properties as the one
 *          above.
 * \~spanish Pregunta al sistema por si mismo.  Mismas propiedades que la de
 *          arriba.
 * \~
 *
 * \~english
 * @par Threads
 * Safe from any thread.
 *
 * \~spanish
 * @par Hilos
 * Segura desde cualquier hilo.
 *
 * \~
 * @return
 * \~english the figures, with a zero in whatever this system does not report.
 * \~spanish las cifras, con un cero en lo que este sistema no diga.
 * \~
 *
 * \~english
 * @code
 *   // Size things by what IS there, not by a hand-written constant.
 *   const auto s = util::os_system_memory();
 *   const size_t cache = s.physical_available / 8;   // an eighth of what's free
 * @endcode
 *
 * \~spanish
 * @code
 *   // Dimensionar segun lo que HAY, no segun una constante escrita a mano.
 *   const auto s = util::os_system_memory();
 *   const size_t cache = s.physical_available / 8;   // un octavo de lo libre
 * @endcode
 *
 * \~
 */
OsSystemMemory os_system_memory() noexcept;

/**
 * @brief
 * \~english Where the executable is loaded.
 * \~spanish Donde esta cargado el ejecutable.
 * \~
 *
 * \~english
 * WHAT FOR.  To dump a code address as BASE + OFFSET instead of absolute.  With
 * address space layout randomisation, one run's absolute address cannot be read
 * with another run's binary: `addr2line` would give another function, or none,
 * and the dump would look correct.
 *
 * It is asked ONCE and kept: it does not change while the process lives.
 *
 * \~spanish
 * PARA QUE.  Para volcar una direccion de codigo como BASE + DESPLAZAMIENTO en
 * vez de absoluta.  Con la disposicion aleatoria del espacio de direcciones, la
 * direccion absoluta de una corrida no se puede leer con el binario de otra:
 * `addr2line` daria otra funcion, o ninguna, y el volcado pareceria correcto.
 *
 * Se pregunta UNA vez y se guarda: no cambia mientras el proceso vive.
 *
 * \~
 * @return
 * \~english the base, or nullptr if the system does not give it -- and then
 *           whoever dumps has to say the addresses are not resolvable, not keep
 *           quiet.
 * \~spanish la base, o nullptr si el sistema no la da -- y entonces quien
 *           vuelque tiene que decir que las direcciones no son resolubles, no
 *           callarse.
 * \~
 *
 * \~english
 * @code
 *   const uintptr_t off = uintptr_t(pc) - uintptr_t(util::os_module_base());
 *   // addr2line -f -C -e binary 0x<off>
 * @endcode
 *
 * \~spanish
 * @code
 *   const uintptr_t off = uintptr_t(pc) - uintptr_t(util::os_module_base());
 *   // addr2line -f -C -e binario 0x<off>
 * @endcode
 *
 * \~
 */
const void *os_module_base() noexcept;

/**
 * @brief
 * \~english Gives the core up to another thread that is ready to run.
 * \~spanish Le cede el nucleo a otro hilo que este listo para correr.
 * \~
 *
 * \~english
 * THIS IS NOT `cpu_relax`, and the difference is the one between waiting and
 * getting in the way.  `cpu_relax` tells the PROCESSOR that this is a wait, but
 * the thread still owns the core; this hands it to somebody else.
 *
 * WHY IT IS NEEDED.  A spin that never yields is only correct while there are
 * fewer spinners than cores.  Past that point the lock holder can lose its
 * processor, and then every other spinner burns its WHOLE quantum waiting on a
 * thread that is not running -- a convoy.  A waiter cannot fix that by spinning
 * faster: the only thing that helps is handing the core back so the holder can
 * reach the release.
 *
 * Measured in this allocator on the shared lock: the knee appeared exactly when
 * spinners went past 24 on a 24-core machine, and from there CPU time per
 * operation grew 19x without doing any more work.
 *
 * Call it ONLY after spinning for a while: yielding to nobody costs a system
 * call, so in the normal case -- little contention -- this is never reached.
 *
 * \~spanish
 * ESTO NO ES `cpu_relax`, y la diferencia es la que hay entre esperar y
 * estorbar.  `cpu_relax` le dice al PROCESADOR que esto es una espera, pero el
 * hilo se queda con el nucleo; esto se lo entrega a otro.
 *
 * POR QUE HACE FALTA.  Un giro que no cede nunca solo es correcto mientras haya
 * menos hilos girando que nucleos.  Pasado ese punto, el que tiene el cerrojo
 * puede perder su procesador, y entonces todos los demas queman su cuanto
 * ENTERO esperando a un hilo que no esta corriendo -- un convoy --.  Quien
 * espera no puede arreglar eso girando mas deprisa: lo unico que ayuda es
 * devolver el nucleo para que el que lo tiene llegue a soltarlo.
 *
 * Medido en este asignador sobre el cerrojo compartido: el codo aparecia
 * exactamente cuando los que giraban pasaban de 24 en una maquina de 24
 * nucleos, y a partir de ahi el tiempo de CPU por operacion crecia 19x sin
 * hacer ni un trabajo mas.
 *
 * Se llama SOLO despues de girar un rato: ceder a nadie cuesta una llamada al
 * sistema, asi que en el caso normal -- poca contencion -- aqui no se llega
 * nunca.
 *
 * \~
 *
 * \~english
 * @code
 *   for (unsigned i = 0; i < kSpins; ++i) {
 *       if (try_take()) return;   // spin first, it is usually enough
 *       cpu_relax();
 *   }
 *   util::os_yield();             // only once spinning has clearly failed
 * @endcode
 *
 * \~spanish
 * @code
 *   for (unsigned i = 0; i < kSpins; ++i) {
 *       if (try_take()) return;   // primero girar, que suele bastar
 *       cpu_relax();
 *   }
 *   util::os_yield();             // solo cuando girar ha fallado claramente
 * @endcode
 *
 * \~
 */
void os_yield() noexcept;


} // namespace util

#endif // VESTA_UTIL_OS_MEMORY_H

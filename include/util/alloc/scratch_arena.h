/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/alloc/scratch_arena.h
 * @brief
 * \~english THROWAWAY memory for one phase, handed out by bumping a pointer.
 * \~spanish Memoria de USAR Y TIRAR para una fase, con reserva a puntero.
 * \~
 *
 * \~english
 * WHAT IT IS FOR.  When all the memory of a phase dies together, a
 * general-purpose allocator is doing work nobody asked for: keeping track of
 * every block so that each one can be returned separately.  Here allocating is
 * bumping a pointer and freeing is nothing at all; the whole phase is released
 * with a single write.  Measured against the project's general allocator: 1.95
 * ns per allocation against 10.03, that is 5.2x (and ~22x against the system
 * `malloc`).
 *
 * WHEN IT DOES **NOT** WORK -- read this before using it.  A matching lifetime
 * is not enough: the allocation PATTERN has to match too.  An arena cannot
 * reclaim anything until the end, and a container that GROWS (`std::vector` and
 * company) allocates a new buffer and abandons the old one on every growth.
 * Putting range analysis in here -- whose lifetime fitted perfectly -- took peak
 * memory from 2,414 MB to 5,455 MB (+126%) in exchange for 4% speed, because
 * there are over a million insertions leaving remains behind.  For small,
 * repeated allocations of containers that grow, the right thing is the
 * size-class allocator in `util/alloc/host_allocator.h`, which DOES take the old
 * buffer back on growth.
 *
 * This arena fits: many small objects of KNOWN size that are never resized,
 * nodes that get linked together, single-pass working buffers.
 *
 * And nothing that comes out of here may outlive its phase: if it escapes, on
 * recycling it is left pointing at reused memory, and that failure shows up
 * miles away from its cause.  Check it BEFORE, not after.
 *
 * MARK AND RETURN, not a plain reset: the current position is noted and later
 * returned to.  That way two nested phases do not step on each other, which a
 * global reset would.
 *
 * It REUSES what is already there: the blocks come from `util/os/os_memory.h`
 * -- the only layer that talks to the system about memory below the allocators
 * -- and each thread's arena is located with `ThreadSlot`, which avoids MinGW's
 * emulated TLS (10.83 ns per access against 0.65).
 *
 * \~spanish
 * PARA QUE SIRVE.  Cuando toda la memoria de una fase muere junta, un asignador
 * de proposito general esta haciendo un trabajo que nadie le ha pedido: llevar
 * la cuenta de cada bloque para poder devolverlo por separado.  Aqui reservar
 * es avanzar un puntero y devolver no es nada; la fase entera se suelta con una
 * escritura.  Medido contra el asignador general del proyecto: 1,95 ns por
 * reserva frente a 10,03, o sea 5,2x (y ~22x frente al `malloc` del sistema).
 *
 * CUANDO **NO** SIRVE -- leer esto antes de usarla.  Que la vida util encaje no
 * basta: tambien tiene que encajar el PATRON de reserva.  Una arena no puede
 * reclamar nada hasta el final, y un contenedor que CRECE (`std::vector` y
 * companyia) reserva un bufer nuevo y abandona el viejo en cada crecimiento.
 * Metiendo el analisis de rangos aqui -- cuya vida util encajaba
 * perfectamente -- el pico de memoria paso de 2.414 MB a 5.455 MB (+126%) a
 * cambio de un 4% de velocidad, porque son mas de un millon de inserciones que
 * van dejando restos.  Para reservas pequenas y repetidas de contenedores que
 * crecen, lo correcto es el asignador por clases de `util/alloc/host_allocator.h`,
 * que al crecer SI devuelve el bufer viejo.
 *
 * Esta arena encaja con: muchos objetos pequenos de tamano CONOCIDO que no se
 * redimensionan, nodos que se enlazan, buferes de trabajo de una pasada.
 *
 * Y nada de lo que salga de aqui puede sobrevivir a su fase: si escapa, al
 * reciclar queda apuntando a memoria reutilizada, y ese fallo aparece lejisimos
 * de su causa.  Comprobarlo ANTES, no despues.
 *
 * MARCA Y VUELTA, no un reinicio a secas: se apunta donde estaba y se vuelve
 * ahi.  Asi dos fases anidadas no se pisan, que un reinicio global si haria.
 *
 * REUSA lo que ya hay: los bloques salen de `util/os/os_memory.h` -- la unica capa
 * que habla de memoria con el sistema por debajo de las reservas -- y la arena
 * de cada hilo se localiza con `ThreadSlot`, que evita la TLS emulada de MinGW
 * (10,83 ns por acceso frente a 0,65).
 *
 * \~
 */
#ifndef VESTA_UTIL_SCRATCH_ARENA_H
#define VESTA_UTIL_SCRATCH_ARENA_H

/* Los permisos y la colocacion de sus bloques son parte de la arena, asi que su
 * vocabulario viene de aqui: `OsProt` y `kOsReadWrite`.  Es la misma capa de la
 * que ya salian los bloques. */
#include "util/os/os_memory.h"

#include <cstddef>
#include <cstdint>
#include <new>

namespace util {

/**
 * @brief
 * \~english Phase arena: allocate by bumping, release the lot in one go.
 * \~spanish Arena de fase: se reserva avanzando y se suelta toda de golpe.
 * \~
 *
 * \~english
 * @par Threads
 * **NO method of this class is thread safe, and that IS the design.**  An arena
 * costs 1.95 ns per allocation because allocating is adding to a pointer;
 * putting an atomic or a lock in there would cost more than all the work it
 * does and would leave it with no reason to exist.
 *
 * Safety comes from somewhere else: **each thread has its own** and finds it
 * with @c scratch_arena().  As long as nobody passes arena pointers between
 * threads, there is nothing to synchronise.
 *
 * What that FORBIDS, and it has to be kept in mind because it raises no error:
 *   - keeping a `ScratchArena&` around and using it from another thread;
 *   - handing the pool work that allocates from an arena taken outside it;
 *   - having an object allocated here be RELEASED by another thread (there is
 *     no freeing, but neither may it still be alive when the phase recycles).
 *
 * \~spanish
 * @par Hilos
 * **NINGUN metodo de esta clase es seguro entre hilos, y es EL DISEÑO.**  Una
 * arena vale 1,95 ns por reserva porque reservar es sumar a un puntero; meter
 * ahi un atomico o un cerrojo costaria mas que todo el trabajo que hace y la
 * dejaria sin motivo para existir.
 *
 * La seguridad sale de otro sitio: **cada hilo tiene la suya** y la encuentra
 * con @c scratch_arena().  Mientras nadie pase punteros a una arena entre
 * hilos, no hay nada que sincronizar.
 *
 * Lo que eso PROHIBE, y hay que tenerlo presente porque no da error:
 *   - guardar una `ScratchArena&` y usarla desde otro hilo;
 *   - repartir por el pool trabajo que reserve de una arena tomada fuera;
 *   - que un objeto reservado aqui lo LIBERE otro hilo (no hay liberar, pero
 *     tampoco puede seguir vivo cuando la fase se recicle).
 *
 * \~
 */
class ScratchArena {
  public:
    /**
     * @brief
     * \~english One big block that pieces get handed out from.
     * \~spanish Un bloque grande de donde se van repartiendo trozos.
     * \~
     */
    struct Block {
        /// \~english the next block of the chain, null at the end
        /// \~spanish el bloque siguiente de la cadena, nulo al final  \~
        Block *next;
        /// \~english usable bytes behind this header
        /// \~spanish bytes utiles detras de esta cabecera  \~
        size_t size;
        /// \~english how many of them are already handed out
        /// \~spanish cuantos de ellos ya estan repartidos  \~
        size_t used;
    };

    /**
     * @brief
     * \~english Where the arena stood, so that it can be returned to.
     * \~spanish Donde estaba la arena, para poder volver.
     * \~
     */
    struct Mark {
        /// \~english the block that was being handed out from
        /// \~spanish el bloque del que se estaba repartiendo  \~
        Block *block;
        /// \~english how much of it was in use at that point
        /// \~spanish cuanto de el estaba en uso en ese momento  \~
        size_t used;
    };

    /// \~english The ordinary arena: readable and writable, wherever it lands.
    /// \~spanish La arena de siempre: se lee y se escribe, y cae donde caiga.
    /// \~
    ScratchArena() noexcept = default;

    /**
     * @brief
     * \~english An arena with the PERMISSIONS asked for, placed CLOSE to an
     *          address.
     * \~spanish Una arena con los PERMISOS que se pidan, colocada CERCA de una
     *          direccion.
     * \~
     *
     * \~english
     * IT IS THE SAME ARENA, and that is the point: reserving a range and
     * handing pieces out of it does not change because the pages are executable
     * or because they have to land somewhere specific.  Only two things it
     * already did implicitly become parameters.
     *
     * WHAT PLACEMENT IS FOR.  Generated code reaches its data with 32-bit
     * displacements, which cover +-2 GB; past that the reference cannot be
     * emitted at all.  So an arena that will hold code has to be able to say
     * "near this datum" -- letting the system choose is fine until something
     * large is reserved in between, and then the answer is arbitrary: measured,
     * 16 GiB from a datum eight bytes off the anchor.
     *
     * If there is no room within @p window the arena is still usable: its
     * blocks then land wherever the system puts them, and @c placed says so.
     * Refusing to hand out memory would turn a placement problem into an
     * out-of-memory one, which is a worse thing to debug.
     *
     * \~spanish
     * ES LA MISMA ARENA, y esa es la idea: apalabrar un rango e ir repartiendo
     * trozos no cambia porque las paginas sean ejecutables ni porque tengan que
     * caer en un sitio concreto.  Solo se vuelven parametros dos cosas que ya
     * hacia de forma implicita.
     *
     * PARA QUE SIRVE LA COLOCACION.  El codigo generado alcanza sus datos con
     * desplazamientos de 32 bits, que cubren +-2 GB; mas alla la referencia no
     * se puede ni emitir.  Asi que una arena que vaya a guardar codigo tiene
     * que poder decir "cerca de este dato" -- dejar elegir al sistema vale
     * hasta que se reserva algo grande por en medio, y entonces la respuesta es
     * arbitraria: medido, a 16 GiB de un dato que estaba a ocho bytes del
     * ancla.
     *
     * Si no hay sitio dentro de @p window la arena sigue sirviendo: sus bloques
     * caen donde el sistema quiera y @c placed lo dice.  Negarse a dar memoria
     * convertiria un problema de colocacion en uno de falta de memoria, que se
     * depura mucho peor.
     *
     * \~
     * @param prot
     * \~english the permissions its blocks are committed with.
     * \~spanish los permisos con los que se comprometen sus bloques.
     * \~
     * @param anchor
     * \~english the address to stay close to, or nullptr for anywhere.
     * \~spanish la direccion de la que no alejarse, o nulo para cualquier
     *           sitio.
     * \~
     * @param window
     * \~english how far from @p anchor still counts as close.
     * \~spanish a que distancia de @p anchor sigue valiendo.
     * \~
     *
     * \~english
     * @code
     *   // An arena for generated code that has to reach the module's globals.
     *   util::ScratchArena code(util::kOsReadWriteExec, globals,
     *                           (size_t(1) << 31) - (128u << 20));
     *   auto *p = static_cast<uint8_t *>(code.allocate(n, 16));
     * @endcode
     *
     * \~spanish
     * @code
     *   // Una arena de codigo generado que tiene que alcanzar los globales.
     *   util::ScratchArena codigo(util::kOsReadWriteExec, globales,
     *                             (size_t(1) << 31) - (128u << 20));
     *   auto *p = static_cast<uint8_t *>(codigo.allocate(n, 16));
     * @endcode
     *
     * \~
     */
    ScratchArena(OsProt prot, const void *anchor, size_t window) noexcept
        : prot_(prot), anchor_(anchor), window_(window) {}

    /**
     * @brief
     * \~english Whether its blocks really landed where they were asked to.
     * \~spanish Si sus bloques cayeron de verdad donde se pidio.
     * \~
     *
     * \~english
     * FALSE IS NOT A FAILURE, it is the answer to a different question: the
     * arena works either way, but code living in it may not reach its data.
     * Asking is the only way to tell "I did not ask for the area" from "I asked
     * and there was none" -- they look identical from the outside and they are
     * different problems.
     *
     * True as well for an arena that never asked for a place: it landed
     * wherever it was going to, which is exactly what it wanted.
     *
     * \~spanish
     * FALSO NO ES UN FALLO, es la respuesta a otra pregunta: la arena sirve
     * igual, pero el codigo que viva en ella puede no alcanzar sus datos.
     * Preguntarlo es la unica forma de distinguir "no pedi la zona" de "pedi y
     * no habia" -- desde fuera se ven igual y son arreglos distintos.
     *
     * Cierto tambien para una arena que no pidio sitio: cayo donde iba a caer,
     * que es justo lo que queria.
     *
     * \~
     * @return
     * \~english true while every block is within the window.
     * \~spanish cierto mientras todos los bloques esten dentro de la ventana.
     * \~
     */
    bool placed() const noexcept { return placed_; }

    /**
     * @brief
     * \~english What the last placement attempt SAW, when it could not place.
     * \~spanish Lo que VIO el ultimo intento de colocacion, cuando no pudo.
     * \~
     *
     * \~english
     * `placed() == false` says it did not fit; this says why, and the two
     * answers lead to different fixes: no regions at all means nothing was
     * asked for, while regions with no room means the window really is full --
     * which is what a large reservation lying across it looks like from here.
     *
     * \~spanish
     * `placed() == false` dice que no cupo; esto dice por que, y las dos
     * respuestas llevan a arreglos distintos: cero regiones significa que no se
     * llego a pedir, y regiones sin sitio significa que la ventana esta llena
     * de verdad -- que es como se ve desde aqui una reserva grande cruzada por
     * en medio.
     *
     * \~
     * @return
     * \~english the regions walked and the largest free run seen.
     * \~spanish las regiones recorridas y el hueco libre mayor visto.
     * \~
     */
    const OsNearScan &last_scan() const noexcept { return scan_; }

    /**
     * @brief
     * \~english Allocates @p n bytes aligned to @p align.
     * \~spanish Reserva @p n bytes alineados a @p align.
     * \~
     *
     * \~english
     * @par Threads
     * **NOT safe**, on purpose: only the thread that owns this arena.
     *
     * \~spanish
     * @par Hilos
     * **NO segura**, a proposito: solo el hilo dueño de esta arena.
     *
     * \~
     * @param n
     * \~english how many bytes are wanted.
     * \~spanish cuantos bytes se quieren.
     * \~
     * @param align
     * \~english the alignment the block must have; a power of two.
     * \~spanish la alineacion que debe tener el bloque; potencia de dos.
     * \~
     * @return
     * \~english the block, or null if the system gave no more memory.
     * \~spanish el bloque, o nulo si el sistema no dio mas memoria.
     * \~
     *
     * \~english
     * @code
     *   // A single-pass working buffer, without going through the allocator.
     *   util::ScratchScope phase;                      // marks on entry
     *   auto *table = static_cast<Node *>(
     *       util::scratch_arena().allocate(n * sizeof(Node), alignof(Node)));
     *   if (table == nullptr) return false;            // out of memory
     *   // ...use `table`; on leaving the scope everything recycles at once.
     * @endcode
     *
     * \~spanish
     * @code
     *   // Un bufer de trabajo de una pasada, sin pasar por el asignador.
     *   util::ScratchScope fase;                       // marca al entrar
     *   auto *tabla = static_cast<Nodo *>(
     *       util::scratch_arena().allocate(n * sizeof(Nodo), alignof(Nodo)));
     *   if (tabla == nullptr) return false;            // sin memoria
     *   // ...usar `tabla`; al salir del ambito se recicla todo de golpe.
     * @endcode
     *
     * \~
     */
    void *allocate(size_t n, size_t align) noexcept;

    /**
     * @brief
     * \~english Notes the current state, so that it can be returned to.
     * \~spanish Apunta el estado actual, para poder volver aqui.
     * \~
     *
     * \~english
     * @par Threads
     * **NOT safe**: only the owning thread.
     *
     * \~spanish
     * @par Hilos
     * **NO segura**: solo el hilo dueño.
     *
     * \~
     * @return
     * \~english the mark to hand to @c release later.
     * \~spanish la marca que se le pasara despues a @c release.
     * \~
     */
    Mark mark() const noexcept {
        return Mark{current_, current_ ? current_->used : 0};
    }

    /**
     * @brief
     * \~english Returns to @p m.  What was allocated after it becomes reusable.
     * \~spanish Vuelve a @p m.  Lo reservado despues queda listo para reusarse.
     * \~
     *
     * \~english
     * Mark and return, NOT a reset: that way two nested phases do not step on
     * each other.
     *
     * @par Threads
     * **NOT safe**: only the owning thread.  And returning with a mark taken in
     * ANOTHER thread is not merely unsafe, it means nothing at all.
     *
     * \~spanish
     * Marca y vuelta, NO un reinicio: asi dos fases anidadas no se pisan.
     *
     * @par Hilos
     * **NO segura**: solo el hilo dueño.  Y volver con una marca tomada en OTRO
     * hilo no es que sea inseguro, es que no significa nada.
     *
     * \~
     * @param m
     * \~english the mark taken with @c mark on this same arena.
     * \~spanish la marca tomada con @c mark sobre esta misma arena.
     * \~
     *
     * \~english
     * @code
     *   // By hand, when the RAII scope does not fit (inside a loop, say).
     *   util::ScratchArena &a = util::scratch_arena();
     *   for (const auto &item : things) {
     *       const auto m = a.mark();
     *       process(item);           // allocates whatever it needs
     *       a.release(m);            // and it recycles on every turn
     *   }
     * @endcode
     *
     * \~spanish
     * @code
     *   // A mano, cuando el ambito RAII no encaja (un bucle, por ejemplo).
     *   util::ScratchArena &a = util::scratch_arena();
     *   for (const auto &item : cosas) {
     *       const auto m = a.mark();
     *       procesar(item);          // reserva lo que le haga falta
     *       a.release(m);            // y se recicla en cada vuelta
     *   }
     * @endcode
     *
     * \~
     */
    void release(Mark m) noexcept;

    /**
     * @brief
     * \~english Bytes asked of the system, not the ones in use.
     * \~spanish Bytes pedidos al sistema, no los que estan en uso.
     * \~
     *
     * \~english
     * @par Threads
     * **NOT safe**: only the owning thread.
     *
     * \~spanish
     * @par Hilos
     * **NO segura**: solo el hilo dueño.
     *
     * \~
     * @return
     * \~english the total of every block asked of the system, which never goes
     *           down: an arena keeps its blocks to reuse them.
     * \~spanish el total de todos los bloques pedidos al sistema, que nunca
     *           baja: la arena se queda los bloques para reusarlos.
     * \~
     *
     * \~english
     * @code
     *   const size_t before = util::scratch_arena().reserved_bytes();
     *   compile(unit);
     *   printf("phase: %zu KiB\n",
     *          (util::scratch_arena().reserved_bytes() - before) / 1024);
     * @endcode
     *
     * \~spanish
     * @code
     *   const size_t antes = util::scratch_arena().reserved_bytes();
     *   compilar(unidad);
     *   printf("fase: %zu KiB\n",
     *          (util::scratch_arena().reserved_bytes() - antes) / 1024);
     * @endcode
     *
     * \~
     */
    size_t reserved_bytes() const noexcept { return reserved_; }

  private:
    Block *add_block(size_t least) noexcept;

    Block *head_ = nullptr;
    Block *current_ = nullptr;
    size_t reserved_ = 0;
    /// \~english what its blocks are committed with; the ordinary arena reads
    ///           and writes.
    /// \~spanish con que se comprometen sus bloques; la arena de siempre lee y
    ///           escribe.
    /// \~
    OsProt prot_ = kOsReadWrite;
    /// \~english the address not to stray from, or null for anywhere
    /// \~spanish la direccion de la que no alejarse, o nulo para cualquier sitio
    /// \~
    const void *anchor_ = nullptr;
    /// \~english how far from @c anchor_ still counts as close
    /// \~spanish a que distancia de @c anchor_ sigue valiendo  \~
    size_t window_ = 0;
    /// \~english false as soon as ONE block lands outside the window; see
    ///           @c placed.
    /// \~spanish falso en cuanto UN bloque cae fuera de la ventana; ver
    ///           @c placed.
    /// \~
    bool placed_ = true;
    /// \~english what the last attempt saw; see @c last_scan.
    /// \~spanish lo que vio el ultimo intento; ver @c last_scan.  \~
    OsNearScan scan_{0, 0};
};

/**
 * @brief
 * \~english THIS thread's arena.  Created the first time it is asked for.
 * \~spanish La arena de ESTE hilo.  Se crea la primera vez que se pide.
 * \~
 *
 * \~english
 * @par Threads
 * **The function IS safe** -- each thread gets its own -- but what it returns is
 * NOT.  It is exactly that split that lets the arena synchronise nothing: the
 * safety is in the handing out, not in the structure.
 *
 * \~spanish
 * @par Hilos
 * **La funcion SI es segura** -- cada hilo recibe la suya --, pero lo que
 * devuelve NO lo es.  Es justo esa division la que permite que la arena no
 * sincronice nada: la seguridad esta en el reparto, no en la estructura.
 *
 * \~
 * @return
 * \~english a reference to the calling thread's arena, valid until the thread
 *           ends.
 * \~spanish una referencia a la arena del hilo que llama, valida hasta que ese
 *           hilo termine.
 * \~
 *
 * \~english
 * @code
 *   // Right: each thread asks for its own INSIDE its task.
 *   pool.enqueue([&] {
 *       util::ScratchScope phase;
 *       work();                   // allocates from THIS worker's arena
 *   });
 *
 *   // WRONG: taking it outside and using it inside is two threads on one arena.
 *   util::ScratchArena &a = util::scratch_arena();
 *   pool.enqueue([&a] { a.allocate(64, 8); });      // NO
 * @endcode
 *
 * \~spanish
 * @code
 *   // Correcto: cada hilo pide la suya DENTRO de su tarea.
 *   pool.enqueue([&] {
 *       util::ScratchScope fase;
 *       trabajar();               // reserva de la arena de ESTE trabajador
 *   });
 *
 *   // MAL: tomarla fuera y usarla dentro son dos hilos sobre una arena.
 *   util::ScratchArena &a = util::scratch_arena();
 *   pool.enqueue([&a] { a.allocate(64, 8); });      // NO
 * @endcode
 *
 * \~
 */
ScratchArena &scratch_arena() noexcept;

/**
 * @brief
 * \~english Marks on entry and returns on exit, come what may.
 * \~spanish Marca al entrar y vuelve al salir, pase lo que pase.
 * \~
 *
 * \~english
 * It is the right way to bound a phase: if the work leaves through an
 * exception, the arena recycles all the same.
 *
 * @par Threads
 * Each object is good for ITS thread.  It binds to the arena of the thread that
 * constructs it, so creating it in one thread and destroying it in another
 * would return somebody else's mark.  That is why it is not copyable.
 *
 * \~spanish
 * Es la forma correcta de acotar una fase: si el trabajo sale por una
 * excepcion, la arena se recicla igual.
 *
 * @par Hilos
 * Cada objeto vale para SU hilo.  Se ata a la arena del hilo que lo construye,
 * asi que crearlo en un hilo y destruirlo en otro devolveria una marca ajena.
 * Por eso no es copiable.
 *
 * \~
 *
 * \~english
 * @code
 *   void optimise(Function &f) {
 *       util::ScratchScope phase;     // marks here
 *       ...                           // allocates whatever it wants
 *   }                                 // and recycles on exit, even if it throws
 * @endcode
 *
 * \~spanish
 * @code
 *   void optimizar(Funcion &f) {
 *       util::ScratchScope fase;      // marca aqui
 *       ...                           // reserva lo que quiera
 *   }                                 // y se recicla al salir, incluso si lanza
 * @endcode
 *
 * \~
 */
class ScratchScope {
  public:
    /**
     * @brief
     * \~english Takes the mark of the calling thread's arena.
     * \~spanish Toma la marca de la arena del hilo que llama.
     * \~
     */
    ScratchScope() noexcept : arena_(scratch_arena()), mark_(arena_.mark()) {}

    /**
     * @brief
     * \~english Returns to the mark taken on construction.
     * \~spanish Vuelve a la marca tomada al construir.
     * \~
     */
    ~ScratchScope() { arena_.release(mark_); }

    ScratchScope(const ScratchScope &) = delete;
    ScratchScope &operator=(const ScratchScope &) = delete;

  private:
    ScratchArena &arena_;
    ScratchArena::Mark mark_;
};

/**
 * @brief
 * \~english Standard allocator backed by the thread's arena.
 * \~spanish Asignador estandar respaldado por la arena del hilo.
 * \~
 *
 * \~english
 * It serves to put a container inside the arena without touching the code that
 * uses it.  `deallocate` does NOTHING on purpose: what gets released is the
 * whole phase, not the objects one by one.
 *
 * MIND the consequence, which is what makes it a bad idea with containers that
 * grow: their old buffers stay until the phase ends.  See the file header.
 *
 * @par Threads
 * **It is not safe**, for the same reason the arena is not: it allocates from
 * the calling thread's one.  And it has a trap of its own -- a container built
 * with this in one thread and used in ANOTHER would allocate from two different
 * arenas, so its old and new buffers would live in phases that recycle
 * separately.  It raises no error; it gives memory trampled on much later.
 *
 * \~spanish
 * Sirve para poner un contenedor dentro de la arena sin tocar el codigo que lo
 * usa.  `deallocate` NO hace nada a proposito: lo que se suelta es la fase
 * entera, no los objetos uno a uno.
 *
 * OJO con la consecuencia, que es la que desaconseja usarlo con contenedores
 * que crecen: sus buferes viejos se quedan hasta que acabe la fase.  Ver la
 * cabecera del fichero.
 *
 * @par Hilos
 * **No es seguro**, por lo mismo que la arena: reserva de la del hilo que
 * llame.  Y hay una trampa propia -- un contenedor construido con esto en un
 * hilo y usado en OTRO reservaria de dos arenas distintas, con lo que sus
 * bufers viejo y nuevo vivirian en fases que se reciclan por separado.  No da
 * error; da memoria pisada mucho despues.
 *
 * \~
 * @tparam T
 * \~english the type of element the container asks for.
 * \~spanish el tipo de elemento que pide el contenedor.
 * \~
 *
 * \~english
 * @code
 *   // A working vector that dies with the phase, of KNOWN size.
 *   util::ScratchScope phase;
 *   std::vector<int, util::ScratchAlloc<int>> v;
 *   v.reserve(n);           // reserve in one go: growing here leaves remains
 *   for (...) v.push_back(x);
 * @endcode
 *
 * \~spanish
 * @code
 *   // Un vector de trabajo que muere con la fase, de tamano CONOCIDO.
 *   util::ScratchScope fase;
 *   std::vector<int, util::ScratchAlloc<int>> v;
 *   v.reserve(n);           // reservar de una vez: crecer aqui deja restos
 *   for (...) v.push_back(x);
 * @endcode
 *
 * \~
 */
template <typename T> class ScratchAlloc {
  public:
    using value_type = T;

    ScratchAlloc() noexcept = default;

    /**
     * @brief
     * \~english Converts from the allocator of another type; they all share the
     *          same arena.
     * \~spanish Convierte desde el asignador de otro tipo; todos comparten la
     *          misma arena.
     * \~
     * @tparam U
     * \~english the other element type.
     * \~spanish el otro tipo de elemento.
     * \~
     */
    template <typename U> ScratchAlloc(const ScratchAlloc<U> &) noexcept {}

    /**
     * @brief
     * \~english Allocates room for @p n elements in the thread's arena.
     * \~spanish Reserva sitio para @p n elementos en la arena del hilo.
     * \~
     * @param n
     * \~english how many elements are wanted.
     * \~spanish cuantos elementos se quieren.
     * \~
     * @return
     * \~english the block, already aligned for @c T.
     * \~spanish el bloque, ya alineado para @c T.
     * \~
     * @throws std::bad_alloc
     * \~english when the arena could not get more memory; the standard
     *           interface has no other way of saying it.
     * \~spanish cuando la arena no pudo conseguir mas memoria; la interfaz
     *           estandar no tiene otra forma de decirlo.
     * \~
     */
    T *allocate(size_t n) {
        void *p = scratch_arena().allocate(n * sizeof(T), alignof(T));
        if (p == nullptr) throw std::bad_alloc();
        return static_cast<T *>(p);
    }

    /**
     * @brief
     * \~english Does nothing: what gets released is the whole phase.
     * \~spanish No hace nada: lo que se suelta es la fase entera.
     * \~
     */
    void deallocate(T *, size_t) noexcept {}

    /**
     * @brief
     * \~english Two of these are always interchangeable.
     * \~spanish Dos de estos son siempre intercambiables.
     * \~
     * @tparam U
     * \~english the element type of the other allocator.
     * \~spanish el tipo de elemento del otro asignador.
     * \~
     * @return
     * \~english always true: they all allocate from the thread's arena.
     * \~spanish siempre true: todos reservan de la arena del hilo.
     * \~
     */
    template <typename U>
    bool operator==(const ScratchAlloc<U> &) const noexcept {
        return true; // todas comparten la arena del hilo
    }

    /**
     * @brief
     * \~english The negation of @c operator==, which is what the standard asks
     *          for.
     * \~spanish La negacion de @c operator==, que es lo que pide el estandar.
     * \~
     * @tparam U
     * \~english the element type of the other allocator.
     * \~spanish el tipo de elemento del otro asignador.
     * \~
     * @return
     * \~english always false.
     * \~spanish siempre false.
     * \~
     */
    template <typename U>
    bool operator!=(const ScratchAlloc<U> &) const noexcept {
        return false;
    }
};

} // namespace util

#endif // VESTA_UTIL_SCRATCH_ARENA_H

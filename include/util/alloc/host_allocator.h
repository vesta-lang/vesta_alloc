/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/alloc/host_allocator.h
 * @brief
 * \~english The host process's own allocator, sitting behind `operator new`.
 * \~spanish Asignador propio del proceso anfitrion, detras de `operator new`.
 * \~
 *
 * \~english
 * WHY IT EXISTS.  When compiling, the system's `malloc`+`free` are 18.5% of the
 * time (measured with VTune over 24k lines: 3.108 s out of 16.795 s) and they
 * are SPREAD OUT -- the largest single site is 8.7% of that figure -- so no
 * local fix moves them.  What moves them is changing the allocator, because
 * that reaches every site at once.
 *
 * And it pays: measured head to head against msvcrt's `malloc` on the
 * compiler's pattern (bursts of small allocations released together), ours runs
 * at 13.5 ns per operation against 44.3 -- **3.3x**.
 *
 * NO LOCKS, BY DESIGN.  Putting a mutex in to make it thread safe would eat
 * exactly the advantage being sought.  Instead each thread has its own free
 * lists and the fast path synchronises NOTHING: taking a block out is two reads
 * and one write over the thread's own memory.
 *
 * FREEING ACROSS THREADS.  This is the case that sinks naive per-thread
 * allocators: what one thread allocates, another releases.  Here nothing is
 * lost and nothing is corrupted.  Every chunk comes out of ONE region reserved
 * up front and aligned, so:
 *
 *   - telling whether a pointer is ours is TWO compares (in the region or not),
 *     with no table to search and no lock to take;
 *   - from a pointer its chunk comes out with a mask, and from the chunk its
 *     header, which says the size and WHO owns it.
 *
 * If whoever frees is not the owner, the block goes onto the owner's atomic
 * stack (one `compare_exchange`, blocking nobody) and the owner picks the whole
 * stack up in one go when it runs out of blocks.  It is the tcmalloc / mimalloc
 * model.
 *
 * WHAT IT DOES NOT DO.  Anything large (above @c kMaxSmall) goes to the system
 * allocator as is: the split into classes adds nothing there and the system
 * already knows how to do it.  It does not give memory back to the system
 * either; it reuses it.
 *
 * IT CANNOT BE TURNED OFF, and it used to be.  The switch existed so that there
 * was something to compare against -- the only way to know whether an allocator
 * improves anything -- and it was the only way there was.  It is not any more:
 * `support/system_alloc.h` reaches the system allocator from INSIDE this
 * process, so both are measured interleaved in the same round, which a second
 * process cannot be.  And the switch was not free: renaming `malloc` happens at
 * LINK time and cannot be undone at run time, so with it set every allocation
 * still arrived here, found the allocator not in force, and was refused -- and
 * the refusal does not return.  The C runtime asked for 105 bytes while
 * starting up and the process died before `main`: the switch that existed to
 * run the control could not run anything.
 *
 *
 * ------------------------------------------------------------------------
 * WHY THE FAST PATH IS INLINE AND LIVES HERE
 * ------------------------------------------------------------------------
 *
 * Because it was not, and it showed.  `operator new` is REPLACEABLE, so it can
 * never be inlined into whoever allocates; what can be done is stop it from
 * calling anybody in turn.  That was not the case: the whole body lived
 * elsewhere and was too big for the compiler to bring in.  Disassembly of the
 * Release object, before this change:
 *
 *     operator new(unsigned long long):
 *         push   %r12
 *         sub    $0x20,%rsp          <- Win64 shadow space
 *         mov    ...,%eax            <- this DID get inlined
 *         test   %eax,%eax
 *         ...
 *         call   host_alloc          <- and here it went away
 *
 * A full call frame on each of the 62 million allocations of one build.
 * Measured with a benchmark that imitates both shapes (`-O3`, lists always
 * hitting):
 *
 *     calling out (as before)         2.323 ns
 *     fast path inline                1.529 ns      -34%
 *
 * Hence the split: **inline only what happens almost always** -- look up the
 * class and take the first block off the list -- and out of line everything
 * else: registering the thread, collecting what others released, asking for a
 * new chunk and the fall back to the system.  Putting more inside gains nothing
 * and fattens every site that allocates.
 *
 * What that forces: `ThreadCache` and the class table have to be VISIBLE.  They
 * are in @c detail and are not API -- nobody outside this file and its `.cpp`
 * should touch them -- and the common geometry (region, chunks, classes) lives
 * apart in `util/alloc/host_allocator_layout.h` because the arena and the
 * diagnostic metadata use it too.
 *
 * The INVARIANT the fast path depends on: **if a thread has a cache, the
 * allocator is active and the class table is built.**  It holds because
 * registering only happens inside the slow path, and that one checks the first
 * and builds the second beforehand.  Thanks to that the fast path looks at no
 * enable flag: a non-null cache is enough.
 *
 * \~spanish
 * POR QUE EXISTE.  Al compilar, `malloc`+`free` del sistema son el 18,5% del
 * tiempo (medido con VTune sobre 24k lineas: 3,108 s de 16,795 s) y estan
 * REPARTIDOS -- el mayor sitio suelto es el 8,7% de esa cifra --, asi que
 * ningun arreglo puntual los mueve.  Lo que los mueve es cambiar el asignador,
 * porque afecta a todos los sitios a la vez.
 *
 * Y compensa: medido cara a cara con el `malloc` de msvcrt en el patron del
 * compilador (rafagas de reservas pequenas que se sueltan juntas), el nuestro
 * va a 13,5 ns por operacion frente a 44,3 -- **3,3x**.
 *
 * SIN CERROJOS, POR DISENO.  Poner un mutex para hacerlo hilo-seguro se comeria
 * justo la ventaja que se busca.  En su lugar cada hilo tiene sus propias
 * listas libres y el camino rapido no sincroniza NADA: sacar un bloque son dos
 * lecturas y una escritura sobre memoria del propio hilo.
 *
 * LIBERAR ENTRE HILOS.  Es el caso que hunde a los asignadores por hilo
 * ingenuos: lo que reserva un hilo lo suelta otro.  Aqui no se pierde ni se
 * corrompe.  Todos los trozos salen de UNA region reservada de antemano y
 * alineada, asi que:
 *
 *   - saber si un puntero es nuestro son DOS comparaciones (esta o no en la
 *     region), sin buscar en ninguna tabla ni tomar ningun cerrojo;
 *   - de un puntero se saca su trozo con una mascara, y del trozo su cabecera,
 *     que dice el tamano y QUIEN lo posee.
 *
 * Si el que libera no es el dueno, el bloque va a una pila atomica del dueno
 * (un `compare_exchange`, sin bloquear a nadie) y el dueno la recoge entera de
 * un golpe cuando se queda sin bloques.  Es el modelo de tcmalloc / mimalloc.
 *
 * QUE NO HACE.  Lo grande (por encima de @c kMaxSmall) va al asignador del
 * sistema tal cual: ahi el reparto en clases no aporta y el sistema ya sabe
 * hacerlo.  Tampoco devuelve memoria al sistema; la reusa.
 *
 * NO SE PUEDE APAGAR, y antes si.  El interruptor existia para tener con que
 * comparar -- la unica forma de saber si un asignador mejora algo -- y era la
 * unica via que habia.  Ya no lo es: `support/system_alloc.h` alcanza el
 * asignador del sistema DESDE DENTRO de este proceso, asi que los dos se miden
 * intercalados en la misma vuelta, que es lo que dos procesos no pueden hacer.
 * Y el interruptor no salia gratis: el renombrado de `malloc` es de tiempo de
 * ENLACE y no se deshace en ejecucion, asi que con el puesto toda reserva
 * seguia llegando aqui, se encontraba con que no estabamos en vigor y se
 * negaba -- y la negativa no retorna.  El runtime de C pedia 105 bytes al
 * arrancar y el proceso moria antes de `main`: el interruptor que existia para
 * poder correr el control impedia correr nada.
 *
 * ------------------------------------------------------------------------
 * POR QUE EL CAMINO RAPIDO ESTA EN LINEA Y AQUI
 * ------------------------------------------------------------------------
 *
 * Porque no lo estaba, y se notaba.  `operator new` es REEMPLAZABLE, asi que
 * nunca se puede meter dentro de quien reserva; lo que si se puede es que ella
 * no llame a su vez a nadie.  No era el caso: el cuerpo entero vivia fuera y
 * era demasiado grande para que el compilador lo metiera dentro.  Desensamblado
 * del objeto de Release, antes de este cambio:
 *
 *     operator new(unsigned long long):
 *         push   %r12
 *         sub    $0x20,%rsp          <- espacio de sombra de Win64
 *         mov    ...,%eax            <- esto SI se inlinaba
 *         test   %eax,%eax
 *         ...
 *         call   host_alloc          <- y aqui se iba fuera
 *
 * Un marco de llamada completo en cada una de los 62 millones de reservas de
 * una compilacion.  Medido con un banco que imita las dos formas (`-O3`, listas
 * acertando siempre):
 *
 *     llamando fuera (como antes)     2,323 ns
 *     camino rapido dentro            1,529 ns      -34%
 *
 * De ahi el reparto: **en linea solo lo que ocurre casi siempre** -- mirar la
 * clase y sacar el primer bloque de la lista --, y fuera de linea todo lo
 * demas: dar de alta el hilo, recoger lo que soltaron otros, pedir un trozo
 * nuevo y la caida al sistema.  Meter mas dentro no gana nada y engorda cada
 * sitio que reserva.
 *
 * Lo que eso obliga: `ThreadCache` y la tabla de clases tienen que ser
 * VISIBLES.  Estan en @c detail y no son API -- nadie fuera de este fichero y
 * de su `.cpp` debe tocarlas --, y la geometria comun (region, trozos, clases)
 * vive aparte en `util/alloc/host_allocator_layout.h` porque tambien la usan la arena
 * y los metadatos de diagnostico.
 *
 * INVARIANTE del que depende el camino rapido: **si un hilo tiene cache, el
 * asignador esta activo y la tabla de clases esta construida.**  Se cumple
 * porque el alta solo ocurre dentro del camino lento, y ese comprueba lo uno y
 * construye lo otro antes.  Gracias a eso el camino rapido no mira ninguna
 * bandera de activacion: le basta con que el cache no sea nulo.
 *
 * \~
 */
#ifndef VESTA_UTIL_HOST_ALLOCATOR_H
#define VESTA_UTIL_HOST_ALLOCATOR_H

#include "util/alloc/alloc_tag.h"
#include "util/alloc/host_allocator_c.h" // ahi se declara `HostAllocStats`, para C
#include "util/alloc/host_allocator_layout.h"
#include "util/alloc/size_buckets.h"
#include "util/os/thread_slot.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace util {

/**
 * @brief
 * \~english Usage counters, for diagnostics.
 * \~spanish Contadores de uso del asignador, para diagnostico.
 * \~
 *
 * \~english
 * They are kept PER THREAD and added up when asked for, so that counting them
 * does not force any synchronisation on the fast path.
 *
 * \~spanish
 * Se llevan POR HILO y se suman al pedirlos, para que contarlos no obligue a
 * sincronizar en el camino rapido.
 *
 * \~
 * @see host_alloc_stats
 */
/*
 * SIN INICIALIZADORES DE MIEMBRO AQUI, Y ESTO NO ES UN DESCUIDO.
 *
 * Poner `= 0` o `= {}` en los campos hace que la estructura deje de tener
 * construccion TRIVIAL, y eso se propaga: `ThreadCache` la contiene, asi que
 * `g_caches` deja de ser un array que el cargador pone a cero y pasa a tener un
 * INICIALIZADOR DINAMICO que corre entre los constructores globales.
 *
 * Y ahi esta la trampa, porque el asignador se usa ANTES.  El estandar no
 * ordena los constructores de unidades distintas: cualquier global de otro
 * fichero que reserve arranca el asignador, que se monta y empieza a servir.
 * Cuando por fin le toca el turno a ESTE fichero, su inicializador hace un
 * `rep stos` sobre `g_caches` y **borra lo que el asignador ya tenia**: las
 * listas libres con sus bloques, el identificador, la marca de usado, los
 * tramos guardados y las cuentas.
 *
 * Medido en el compilador: 13.702 reservas atendidas y contadas, y de golpe
 * todo a cero.  Los bloques que colgaban de esas listas se pierden -- nadie los
 * vuelve a ver -- y el hilo sigue apuntando a un cache recien borrado.  No
 * fallaba nada: el programa seguia, reservando de nuevo lo perdido, y las
 * cifras que publicaba el asignador contaban desde ese punto como si el
 * arranque no hubiera existido.
 *
 * Con la estructura trivial, `g_caches` vive en `.bss`, lo pone a cero el
 * cargador antes de que corra una sola instruccion, y no hay ningun momento en
 * el que alguien pueda borrarlo.  Quien necesite una copia a cero en la pila
 * que la pida: `HostAllocStats t{};`.
 */
/* SE DECLARA EN LA CABECERA DE C y aqui solo se nombra otra vez.  Cada campo
 * esta documentado alli.  Declararla dos veces -- una por lenguaje -- es la
 * duplicacion que nadie ve romperse: el dia que se le anade un contador a una,
 * la otra sigue compilando y empieza a leer una estructura distinta.
 *
 * Y sigue siendo un POD sin inicializadores de miembro, que es lo que la nota
 * de arriba exige: un `typedef struct` de C no puede tenerlos, asi que la
 * propiedad se cumple por construccion en vez de por acuerdo. */
using HostAllocStats = ::VestaHostAllocStats;

static_assert(AllocTag::kSlots == VESTA_ALLOC_TAG_SLOTS,
              "las etiquetas de C y las de C++ tienen que ser las mismas");
static_assert(kAllocFillSlots == VESTA_ALLOC_FILL_SLOTS,
              "el eje de cuanto se toca de C y el de C++ tienen que ser el "
              "mismo");
static_assert(kSizeBuckets == VESTA_ALLOC_SIZE_BUCKETS,
              "el reparto de tamanos de C y el de C++ tienen que ser el mismo");

namespace detail {

/**
 * @brief
 * \~english One thread's free lists.  All POD: zeroed without running code.
 * \~spanish Listas libres de un hilo.  Todo POD: se inicializa a cero sin
 *          codigo.
 * \~
 *
 * \~english
 * It is not API.  It is in the header solely because the fast path touches it
 * and has to be able to be inline; see the note at the top of the file.
 *
 * \~spanish
 * No es API.  Esta en la cabecera unicamente porque el camino rapido la toca y
 * tiene que poder estar en linea; ver la nota del principio del fichero.
 *
 * \~
 */
struct alignas(64) ThreadCache {
    void *free_list[kClasses];
    uint32_t id;
    bool used;
    /**
     * @brief
     * \~english The tag running RIGHT NOW in this thread, packed.
     * \~spanish La etiqueta que corre AHORA en este hilo, empaquetada.
     * \~
     *
     * \~english
     * Placed next to @c id and @c used on purpose: the fast path already loads
     * that cache line to reach the lists, so reading it costs no new access.
     * Zero is "unknown", which is what an uninitialised POD gives.  @c
     * AllocScope moves it; it is never touched by hand.
     *
     * \~spanish
     * Puesta junto a @c id y @c used a proposito: el camino rapido ya carga esa
     * linea de cache para llegar a las listas, asi que leerla no cuesta ningun
     * acceso nuevo.  Cero es "no se", que es lo que da un POD sin inicializar.
     * La mueve @c AllocScope, nunca se toca a mano.
     *
     * \~
     */
    uint8_t tag;
    /**
     * @brief
     * \~english How much of what it asks for this thread is going to touch.
     * \~spanish Cuanto de lo que pide va a tocar este hilo.
     * \~
     *
     * \~english
     * An @c AllocFill, and it sits HERE because it is free here: @c tag left
     * the structure with padding behind it, so this byte costs no size and
     * moves nothing -- checked with @c sizeof, which did not change.  That
     * matters more than it sounds: the last time anything was added at the
     * front of this structure, an unrelated row went from 4.19 to 11.18 ns.
     *
     * Zero is "nobody said".  @c AllocScope moves it; never by hand.
     *
     * \~spanish
     * Un @c AllocFill, y esta AQUI porque aqui sale gratis: @c tag dejaba
     * relleno detras, asi que este byte no cuesta tamano y no mueve nada --
     * comprobado con @c sizeof, que no cambio --.  Importa mas de lo que
     * parece: la ultima vez que se anadio algo al principio de esta
     * estructura, una fila que no tenia nada que ver paso de 4,19 a 11,18 ns.
     *
     * Cero es "nadie lo dijo".  La mueve @c AllocScope, nunca a mano.
     *
     * \~
     */
    uint8_t fill;
    /**
     * @brief
     * \~english The BIG chunk this thread is spending, per class.
     * \~spanish El trozo GRANDE que este hilo esta gastando, por clase.
     * \~
     *
     * \~english
     * A big chunk is 1 MiB, and chaining its free list writes a pointer into
     * EVERY block -- that is, it touches the whole thing and commits the whole
     * thing.  Measured: a single 16 KiB allocation left 759 KiB resident,
     * twelve times what it used to.  That is why it is chained in BATCHES: what
     * is going to be handed out gets committed and chained, and this remembers
     * which chunk to carry on from.  The waste is still amortised over the
     * whole 1 MiB, which is the reason for the chunk being big.
     *
     * Null while that class has not been touched, which is the normal state:
     * almost nobody uses them all.
     *
     * \~spanish
     * Un trozo grande es de 1 MiB, y encadenar su lista libre escribe un
     * puntero en CADA bloque -- o sea que lo toca entero y lo compromete
     * entero --.  Medido: una sola reserva de 16 KiB dejaba 759 KiB residentes,
     * doce veces lo de antes.  Por eso se encadena por TANDAS: se compromete y
     * se encadena lo que se va a entregar, y aqui se recuerda de que trozo
     * seguir.  El desperdicio se sigue amortizando sobre el 1 MiB entero, que
     * es la razon de que el trozo sea grande.
     *
     * Nulo mientras esa clase no se haya tocado, que es lo normal: casi ninguna
     * las usa todas.
     *
     * \~
     */
    ChunkHeader *big_run[kClasses];
    /**
     * @brief
     * \~english Spans this thread is holding back, chained, by chunk count.
     * \~spanish Tramos que este hilo se guarda, encadenados, por numero de
     *          trozos.
     * \~
     *
     * \~english
     * The index is `chunks - 1`, and null means none -- which is the normal
     * state of a thread that asks for no large blocks.  It exists because the
     * span lock was 76% of what a large allocation cost, and it was the only
     * place where the fast path still synchronised.
     *
     * A CHAIN AND NOT A SINGLE SLOT.  With one span per size, a thread that
     * juggles three buffers of the same size caches one of them and takes the
     * lock for the other two, which is the common shape -- a working set gets
     * replaced, not swapped one at a time.  The link lives in the span's own
     * `SpanNode` area, free to use because a held-back span is in no shared
     * list: nobody else can see it, so nothing has to be excluded.
     *
     * How much is held back is bounded by `span_cache_bytes`, not by the number
     * of slots.  @see kSpanCacheBytes.
     *
     * \~spanish
     * El indice es `trozos - 1`, y nulo significa ninguno -- que es el estado
     * normal de un hilo que no pide bloques grandes --.  Existe porque el
     * cerrojo de los tramos era el 76% de lo que costaba una reserva grande, y
     * era el unico sitio donde el camino rapido seguia sincronizando.
     *
     * UNA CADENA Y NO UNA SOLA RANURA.  Con un tramo por tamano, un hilo que
     * maneja tres buferes del mismo tamano se guarda uno y toma el cerrojo para
     * los otros dos, que es la forma habitual -- un conjunto de trabajo se
     * sustituye, no se cambia de uno en uno --.  El enlace vive en la zona
     * `SpanNode` del propio tramo, libre para usar porque un tramo guardado no
     * esta en ninguna lista compartida: nadie mas puede verlo, asi que no hay
     * nada que excluir.
     *
     * Cuanto se guarda lo limita `span_cache_bytes`, no el numero de ranuras.
     * @see kSpanCacheBytes.
     *
     * \~
     */
    ChunkHeader *span_cache[kSpanCacheSlots];
    /// \~english Bytes currently held back in `span_cache`, to enforce the
    ///           budget.
    /// \~spanish Bytes guardados ahora mismo en `span_cache`, para respetar el
    ///           presupuesto.
    /// \~
    size_t span_cache_bytes;
    /// \~english This thread's share of the counters; they are added up when
    ///           somebody asks.
    /// \~spanish La parte de los contadores de este hilo; se suman cuando
    ///           alguien pregunta.
    /// \~
    HostAllocStats stats;
    /**
     * \~english
     * @brief The hot batch: blocks of ONE class, handed out by index so that an
     *        allocation does not have to load the previous block to know where
     *        the next one is.  See @c kBatchSlots and @c pop_block.
     *
     * AT THE END, and that is not tidiness.  Put at the FRONT it shifted every
     * field after it by 72 bytes -- not a multiple of the line -- and everything
     * else landed differently inside its cache line.  The span cache moved with
     * it, and the churn row at 1 MiB, which does not touch this batch at all,
     * went from 4.19 to 11.18 ns.  Appending leaves every existing offset where
     * it was.
     *
     * \~spanish
     * @brief El lote caliente: bloques de UNA clase, entregados por indice para
     *        que una reserva no tenga que cargar el bloque anterior para saber
     *        donde esta el siguiente.  Ver @c kBatchSlots y @c pop_block.
     *
     * AL FINAL, y no es por orden.  Puesto al PRINCIPIO desplazaba 72 bytes
     * todos los campos de detras -- que no son multiplo de la linea -- y todo lo
     * demas caia distinto dentro de su linea de cache.  El cache de tramos se
     * movio con ellos, y la fila de `churn` a 1 MiB, que no toca este lote para
     * nada, paso de 4,19 a 11,18 ns.  Anadiendo al final, cada campo que ya
     * existia se queda donde estaba.
     * \~
     */
    void *batch[kBatchSlots];
    /// \~english How many are in it.  \~spanish Cuantos hay.  \~
    uint32_t batch_n;
    /// \~english Which class they belong to.  A pop of another class flushes
    ///           what is left back to ITS list first; see @c pop_block_refill.
    /// \~spanish De que clase son.  Sacar de otra clase devuelve antes lo que
    ///           queda a SU lista; ver @c pop_block_refill.
    /// \~
    uint32_t batch_cls;
};

/**
 * EL GUARDIAN DE LO ANTERIOR, y no es una formalidad: esto ya se rompio.
 *
 * Mientras estas dos sean de construccion TRIVIAL, el array de caches vive en
 * `.bss` y lo pone a cero el cargador, antes de que corra una sola instruccion
 * del programa.  En cuanto alguien le pone un `= 0` a un campo, dejan de serlo
 * y el array pasa a tener un inicializador DINAMICO que corre entre los
 * constructores globales -- y el asignador ya esta sirviendo para entonces,
 * porque los globales de otras unidades reservan y el estandar no ordena entre
 * unidades --.  Resultado medido: 13.702 reservas atendidas y luego un
 * `rep stos` que se lleva las listas libres, los identificadores y las cuentas.
 *
 * No fallaba nada.  Se perdian los bloques que colgaban de esas listas y las
 * cifras contaban desde ahi como si el arranque no hubiera existido.  Por eso
 * la comprobacion es de COMPILACION: un test tendria que depender del orden de
 * inicializacion entre unidades, que es justo lo que no esta definido.
 */
static_assert(__is_trivially_constructible(HostAllocStats),
              "HostAllocStats must be constructible without running code: "
              "otherwise the cache array is initialised AFTER the allocator is "
              "already in use, and wipes what it had");
static_assert(__is_trivially_constructible(ThreadCache),
              "ThreadCache must be constructible without running code: see the "
              "note above, this has been broken once already");

/**
 * @brief
 * \~english The size of this structure is MEASURED.  It cannot change quietly.
 * \~spanish El tamano de esta estructura esta MEDIDO.  No puede cambiar en
 *          silencio.
 * \~
 *
 * \~english
 * A CHECK AND NOT A COMMENT, because it already drifted once without a word: a
 * counter added to @c HostAllocStats -- which sits ahead of the hot batch --
 * grew this by 64 bytes and pushed the batch 32 along.  There are
 * @c kTotalCaches of these, so those 64 bytes are 64 KiB of static memory.
 *
 * WHAT IT IS NOT PROTECTING, because it was measured and it is not there:
 * cache-line effects.  The batch straddles two lines and the obvious worry was
 * split accesses or false sharing between threads.  Profiled with hardware
 * counters on twelve threads, three interleaved runs of each layout:
 * @c SPLIT_LOADS and @c SPLIT_STORES came out at exactly zero, every
 * @c XSNP_* event at zero, and @c L3 Bound at 0.0-0.1% -- there is nothing
 * there.  Aligning the array to 64 was also tried and changed none of it.  So
 * the line the batch starts on is FREE to move; what is not free is this
 * structure growing without anyone noticing.
 *
 * IF THIS FAILS: check what was added and whether it belongs in the per-thread
 * cache at all.  A counter touched once per big allocation does not -- it goes
 * in a global, like @c g_by_fill.  Then measure and update the number.
 *
 * \~spanish
 * UNA COMPROBACION Y NO UN COMENTARIO, porque ya se movio una vez sin decir
 * nada: un contador anadido a @c HostAllocStats -- que va por delante del lote
 * caliente -- engordo esto 64 bytes y empujo el lote otros 32.  Hay
 * @c kTotalCaches de estas, asi que esos 64 bytes son 64 KiB de memoria
 * estatica.
 *
 * LO QUE NO PROTEGE, porque se midio y no esta ahi: los efectos de linea de
 * cache.  El lote cruza dos lineas y lo que se temia eran accesos partidos o
 * comparticion falsa entre hilos.  Perfilado con contadores hardware sobre doce
 * hilos, tres corridas intercaladas de cada disposicion: @c SPLIT_LOADS y
 * @c SPLIT_STORES salieron exactamente a cero, todos los eventos @c XSNP_* a
 * cero, y @c L3 Bound al 0,0-0,1% -- ahi no hay nada.  Alinear el array a 64
 * tambien se probo y no cambio ninguno.  Asi que la linea donde empieza el lote
 * PUEDE moverse; lo que no puede es que esta estructura crezca sin que nadie se
 * entere.
 *
 * SI ESTO FALLA: mira que se ha anadido y si tiene sitio en el cache por hilo.
 * Un contador que se toca una vez por reserva grande no lo tiene -- va en un
 * global, como @c g_by_fill --.  Luego mide y actualiza el numero.
 *
 * \~
 */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
/* La cifra es de 64 bits, que es donde se midio.  En 32 los punteros miden la
 * mitad y el tamano es otro; comprobarlo ahi con este numero seria romper el
 * build por una medida que no se ha hecho, que es peor que no comprobar. */
static_assert(sizeof(ThreadCache) == 1216,
              "ThreadCache changed size: there are kTotalCaches of these, so "
              "every 64 bytes is 64 KiB of static memory.  See the note above "
              "before updating this number");
#endif

/// \~english The per-thread slot where the pointer to the cache lives.  See
///           `util/os/thread_slot.h`.
/// \~spanish La ranura por hilo donde vive el puntero al cache.  Ver
///           `util/os/thread_slot.h`.
/// \~
extern ThreadSlot g_cache_slot;

/// \~english Whether the size split is kept.  Looked at once; counting cannot
///           be free, but NOT counting can.
/// \~spanish Si se lleva el reparto de tamanos.  Se mira una vez; contar no
///           puede salir gratis, pero NO contar si.
/// \~
extern bool g_measure;

/**
 * @brief
 * \~english THIS thread's cache, or nullptr if it has none yet.
 * \~spanish El cache de ESTE hilo, o nullptr si todavia no tiene.
 * \~
 *
 * \~english
 * Null means three things at once -- not registered, allocator off, or more
 * threads than slots -- and all three are resolved the same way: through the
 * slow path.  A single compare covering all three cases is what leaves the fast
 * path with no flag to look at.
 *
 * @par Threads
 * Safe.  It reads a per-thread slot, so each thread sees its own and there is
 * nothing to share.
 *
 * \~spanish
 * Nulo significa tres cosas a la vez -- sin dar de alta, asignador apagado, o
 * mas hilos que ranuras -- y las tres se resuelven igual: por el camino lento.
 * Que una sola comparacion cubra los tres casos es lo que deja el camino rapido
 * sin ninguna bandera que mirar.
 *
 * @par Hilos
 * Segura.  Lee una ranura por hilo, asi que cada hilo ve la suya y no hay nada
 * que compartir.
 *
 * \~
 * @return
 * \~english the calling thread's cache, or null if it does not have one.
 * \~spanish el cache del hilo que llama, o nulo si no tiene.
 * \~
 */
#if VESTA_ALLOC_DIRECT_CACHE_TLS
/**
 * @brief
 * \~english THIS thread's cache, in a thread variable of its own.
 * \~spanish El cache de ESTE hilo, en una variable de hilo propia.
 * \~
 *
 * \~english
 * WHY, and it comes out of counting the chain.  This path is not limited by the
 * number of instructions but by the LATENCY of loads that depend on one
 * another, and through the general slot there were four: slot index -> pointer
 * to the cache (indexed by the index) -> the class list -> the block.  glibc's
 * `tcache` does three, because its pointer is directly in the thread variable.
 *
 * This is that one load fewer.  The general slot still exists and is still the
 * right one for everything else -- it is what allows several independent users
 * -- but the allocator is the only one reading it on the hottest path of the
 * process, and there one indirection counts.
 *
 * It is kept IN PARALLEL with the slot, not instead of it: whoever asks the
 * slot has to keep seeing the same thing.
 *
 * \~spanish
 * POR QUE, y sale de contar la cadena.  Este camino no lo limita el numero de
 * instrucciones sino la LATENCIA de las cargas que dependen unas de otras, y
 * por la ranura general iban cuatro: indice de ranura -> puntero al cache
 * (indexado por el indice) -> lista de la clase -> el bloque.  El `tcache` de
 * glibc hace tres, porque su puntero esta directamente en la variable de hilo.
 *
 * Esta es esa carga de menos.  La ranura general sigue existiendo y sigue
 * siendo la buena para todo lo demas -- es lo que permite que haya varios
 * usuarios independientes --; lo que pasa es que el asignador es el unico que
 * la lee en el camino mas caliente del proceso, y ahi una indireccion cuenta.
 *
 * Se mantiene EN PARALELO a la ranura, no en su lugar: quien pregunte por la
 * ranura tiene que seguir viendo lo mismo.
 *
 * \~
 */
extern __thread ThreadCache *g_cache_direct
    __attribute__((tls_model("initial-exec")));
#endif

[[gnu::always_inline]] inline ThreadCache *current_cache() noexcept {
#if VESTA_ALLOC_DIRECT_CACHE_TLS
    return g_cache_direct;
#else
    /* SIN `ensure` aqui, y no es un olvido.  Reservar la ranura es cosa del
     * camino LENTO, que es quien la usa: @c cache lo hace antes de dar de alta.
     * Aqui `ensure` no cambiaba ninguna respuesta -- con la ranura sin reservar,
     * @c get devuelve nulo igual, y con ella reservada pero vacia tambien --,
     * solo anadia una carga atomica mas de la MISMA variable en el camino mas
     * caliente que tiene el compilador.  Y son dos cargas y no una porque son
     * atomicas con orden de adquisicion: el compilador no puede fusionarlas. */
    return static_cast<ThreadCache *>(g_cache_slot.get());
#endif
}

/**
 * \~english
 * @brief What the slot holds between the thread's exit notice and its last
 *        breath.
 *
 * THE PROBLEM IT SOLVES.  A thread gets ONE exit notice, and that notice is
 * where its owner id goes back to the pool.  But the notice is not the last
 * thing that happens to a dying thread: the C runtime keeps tearing it down
 * afterwards, and on Windows that teardown ALLOCATES -- MinGW's thread-exit
 * callback reads its list of `thread_local` destructors through emulated TLS,
 * and reading an emulated TLS variable whose array has just been destroyed
 * builds it again with `calloc`.  Which, since this library is the C runtime's
 * allocator too, arrives here.
 *
 * With the slot merely cleared, that allocation found no cache and took a FRESH
 * id -- correct for safety, since two owners of one cache would corrupt it, but
 * that id can never come back: the one notice was already spent.  One id lost
 * per thread, out of 63, so any program that makes and joins threads ran out
 * and from then on served EVERY allocation of EVERY thread from the shared
 * lists behind the lock.  Measured, 1.73 -> 447.86 ns per operation.
 *
 * WHY A SENTINEL AND NOT ANOTHER FLAG.  The fast path already asks one question
 * about this pointer -- "is there a cache?" -- and a separate flag would make it
 * ask two.  A non-null marker below every real address turns the existing test
 * for null into an unsigned test for "at most the marker", which is the same
 * one instruction.  It is the trick this file already uses twice: `n - 1 >=
 * kMaxSmall` covers zero and overflow together, and `s < kDirectSlots` covers
 * "no slot" and "not direct" together.
 *
 * The value is 1 on purpose.  A reader that forgets to ask dereferences address
 * one and dies immediately and loudly, which is what should happen; a sentinel
 * that pointed at real memory would let the mistake through and corrupt
 * something far away.
 *
 * \~spanish
 * @brief Lo que guarda la ranura entre el aviso de fin de hilo y su ultimo
 *        suspiro.
 *
 * EL PROBLEMA QUE RESUELVE.  Un hilo tiene UN aviso de fin, y en ese aviso es
 * donde su identificador de dueno vuelve al mostrador.  Pero el aviso no es lo
 * ultimo que le pasa a un hilo que muere: el runtime de C sigue desmontandolo
 * despues, y en Windows ese desmontaje RESERVA -- el callback de fin de hilo de
 * MinGW lee su lista de destructores de `thread_local` por TLS emulada, y leer
 * una variable de TLS emulada cuyo array acaban de destruir lo vuelve a
 * construir con `calloc` --.  Que, siendo esta libreria tambien el asignador
 * del runtime de C, acaba aqui.
 *
 * Con la ranura simplemente limpiada, esa reserva no encontraba cache y cogia
 * un identificador NUEVO -- correcto para la seguridad, porque dos duenos de un
 * cache lo corromperian, pero ese identificador ya no puede volver: el unico
 * aviso estaba gastado.  Uno perdido por hilo, de 63, asi que cualquier
 * programa que cree y espere hilos se quedaba sin ellos y desde ahi servia
 * TODAS las reservas de TODOS los hilos por las listas compartidas detras del
 * cerrojo.  Medido, 1,73 -> 447,86 ns por operacion.
 *
 * POR QUE UNA MARCA Y NO OTRA BANDERA.  El camino rapido ya hace una pregunta
 * sobre este puntero -- "hay cache?" -- y una bandera aparte le haria hacer
 * dos.  Una marca no nula por debajo de cualquier direccion real convierte la
 * comprobacion de nulo que ya existe en una comprobacion sin signo de "como
 * mucho la marca", que es la MISMA instruccion.  Es el truco que este fichero
 * ya usa dos veces: `n - 1 >= kMaxSmall` cubre el cero y el desbordamiento a la
 * vez, y `s < kDirectSlots` cubre "sin ranura" y "no directa" a la vez.
 *
 * El valor es 1 a proposito.  Quien se olvide de preguntar desreferencia la
 * direccion uno y muere en el acto y a gritos, que es lo que tiene que pasar;
 * una marca que apuntara a memoria de verdad dejaria pasar el error y
 * corromperia algo lejos de aqui.
 * \~
 */
constexpr uintptr_t kDyingCache = 1;

/**
 * \~english
 * @brief Whether @p c is a cache this thread may actually use.
 *
 * Says no both when there is no cache and when the thread has already had its
 * exit notice.  ONE unsigned comparison for the two, so the fast path costs
 * exactly what it cost when it only asked about null.
 *
 * @code
 * detail::ThreadCache *c = detail::current_cache();
 * if (!detail::have_cache(c)) return detail::host_alloc_slow(n);
 * @endcode
 *
 * \~spanish
 * @brief Si @p c es un cache que este hilo puede usar de verdad.
 *
 * Dice que no tanto cuando no hay cache como cuando el hilo ya recibio su aviso
 * de fin.  UNA comparacion sin signo para las dos, asi que el camino rapido
 * cuesta exactamente lo que costaba cuando solo preguntaba por el nulo.
 *
 * @code
 * detail::ThreadCache *c = detail::current_cache();
 * if (!detail::have_cache(c)) return detail::host_alloc_slow(n);
 * @endcode
 * \~
 *
 * @param c Lo que devolvio @c current_cache.
 * @return true si se puede usar.
 */
[[gnu::always_inline]] inline bool have_cache(const ThreadCache *c) noexcept {
    return reinterpret_cast<uintptr_t>(c) > kDyingCache;
}

/**
 * @brief
 * \~english Takes a block off the list of class @p k.
 * \~spanish Saca un bloque de la lista de la clase @p k.
 * \~
 *
 * \~english
 * It is the CORE, and it is separate on purpose.  Between today's version -- a
 * cache per thread, with frees that may come from another one -- and a
 * single-owner version with nothing to synchronise, what changes is HOW the
 * cache is reached and what freeing needs; this is identical in both.  Pulling
 * it out now costs not one instruction and saves duplicating it later.
 *
 * The first field of a free block IS the link to the next one: the list spends
 * no memory of its own.
 *
 * @par Threads
 * **It is NOT safe, and that IS the design.**  There is not one atomic nor one
 * barrier here: taking a block out is two reads and one write over the thread's
 * own memory.  Putting synchronisation in would eat exactly the advantage being
 * sought -- it is 3 ns per operation -- so instead of protecting the structure,
 * it is guaranteed that nobody else touches it.
 *
 * The caller has to meet ONE of these two:
 *   - @p c is THIS thread's cache (@c current_cache), or
 *   - @p c is the shared cache and its lock is held (see `alloc_shared` in the
 *     `.cpp`).
 *
 * What arrives from other threads does NOT come in through here: it goes onto a
 * separate atomic stack and its owner picks the whole thing up when it runs out
 * of blocks.
 *
 * \~spanish
 * Es el NUCLEO, y esta suelto a proposito.  Entre la version de hoy -- un cache
 * por hilo, con liberaciones que pueden venir de otro -- y una version de un
 * solo dueño, sin nada que sincronizar, lo que cambia es COMO se llega al cache
 * y que hace falta al liberar; esto es identico en las dos.  Sacarlo aparte
 * ahora no cuesta ni una instruccion y evita duplicarlo despues.
 *
 * El primer campo de un bloque libre ES el enlace al siguiente: la lista no
 * gasta memoria aparte.
 *
 * @par Hilos
 * **NO es segura, y eso es EL DISEÑO.**  Aqui no hay ni un atomico ni una
 * barrera: sacar un bloque son dos lecturas y una escritura sobre memoria del
 * propio hilo.  Poner sincronizacion se comeria justo la ventaja que se busca
 * -- son 3 ns por operacion --, asi que en vez de proteger la estructura se
 * garantiza que nadie mas la toca.
 *
 * Quien llame tiene que cumplir UNA de estas dos:
 *   - @p c es el cache de ESTE hilo (@c current_cache), o
 *   - @p c es el cache compartido y se tiene su cerrojo (ver `alloc_shared` en
 *     el `.cpp`).
 *
 * Lo que llega de otros hilos NO entra por aqui: va a una pila atomica aparte
 * y su dueño la recoge entera cuando se queda sin bloques.
 *
 * \~
 * @param c
 * \~english the cache to take it from, under the conditions above.
 * \~spanish el cache del que sacarlo, con las condiciones de arriba.
 * \~
 * @param k
 * \~english the size class, in [0, @c kClasses).
 * \~spanish la clase de tamano, en [0, @c kClasses).
 * \~
 * @return
 * \~english the block, or nullptr if that list was empty.
 * \~spanish el bloque, o nullptr si esa lista estaba vacia.
 * \~
 */
/// \~english Fills the batch from the class's list, switching class if needed.
///           Out of line: it runs once every @c kBatchSlots allocations.
/// \~spanish Llena el lote desde la lista de la clase, cambiando de clase si
///           hace falta.  Fuera de linea: corre una vez cada @c kBatchSlots
///           reservas.
/// \~
void *pop_block_refill(ThreadCache *c, uint32_t k) noexcept;

[[gnu::always_inline]] inline void *pop_block(ThreadCache *c,
                                              uint32_t k) noexcept {
    /* FROM THE BATCH, BY INDEX.  The list is still there and still the truth;
     * what changed is that a run of allocations in one class no longer walks it
     * one dependent load at a time.  See @c kBatchSlots. */
    /* THE CONSTANT IS ASKED FIRST, and the order is the whole point.  A class
     * without a batch must not so much as LOOK at `batch_cls`: those fields sit
     * at the end of the cache, `free_list` at the start, so touching them makes
     * a big-class allocation read TWO lines where it used to read one.  Asking
     * `k <= kBatchMaxClass` costs nothing -- a compare against a compile-time
     * constant, no memory at all -- and keeps those classes exactly as they
     * were before the batch existed.
     *
     * Measured with the test the other way round: `calloc` at 256 bytes, which
     * is above the threshold and should not have moved, went from 3.02 to 4.05
     * ns.  Nothing was going to the system -- the benchmark's own committed
     * counter reads 0.00 for that row in both -- so it was this. */
    if (__builtin_expect(k <= kBatchMaxClass, 1)) {
        if (__builtin_expect(c->batch_cls == k && c->batch_n != 0, 1)) {
            void *p = c->batch[--c->batch_n];
            // Contar por etiqueta ES contar: el total sale de sumar esta tabla,
            // asi que saber el proposito no anade ni una instruccion.
            c->stats.by_tag[c->tag]++;
            return p;
        }
        return pop_block_refill(c, k);
    }

    /* Y las clases grandes, exactamente como estaban: la lista, en linea.
     * Mandarlas al camino de recarga ponia una LLAMADA en cada reserva suya, y
     * se noto en el acto -- `churn` a 256 bytes paso de 1,22 a 1,76 ns. */
    void *p = c->free_list[k];
    if (p == nullptr) return nullptr;
    c->free_list[k] = *reinterpret_cast<void **>(p);
    c->stats.by_tag[c->tag]++;
    return p;
}

/**
 * @brief
 * \~english Puts a block back on its class's list.  The other half of the core.
 * \~spanish Devuelve un bloque a la lista de su clase.  La otra mitad del
 *          nucleo.
 * \~
 *
 * \~english
 * @par Threads
 * **NOT safe, on purpose.**  The same conditions as @c pop_block.
 *
 * \~spanish
 * @par Hilos
 * **NO es segura, a proposito.**  Mismas condiciones que @c pop_block.
 *
 * \~
 * @param c
 * \~english the cache to put it back into.
 * \~spanish el cache al que devolverlo.
 * \~
 * @param p
 * \~english the block, which stops being usable from here on.
 * \~spanish el bloque, que deja de poder usarse a partir de aqui.
 * \~
 * @param k
 * \~english its size class.
 * \~spanish su clase de tamano.
 * \~
 */
[[gnu::always_inline]] inline void push_block(ThreadCache *c, void *p,
                                              uint32_t k) noexcept {
    c->stats.small_frees++;
    /* INTO THE BATCH when it belongs there and there is room: one store to an
     * array we are already touching, instead of a store INTO the block, which
     * is a line the caller may have finished with a while ago.  See
     * @c kBatchSlots.
     *
     * The constant goes first for the same reason as in @c pop_block: a class
     * without a batch must not read `batch_cls` at all. */
    if (__builtin_expect(k <= kBatchMaxClass, 1) &&
        __builtin_expect(c->batch_cls == k && c->batch_n < kBatchSlots, 1)) {
        c->batch[c->batch_n++] = p;
        return;
    }
    *reinterpret_cast<void **>(p) = c->free_list[k];
    c->free_list[k] = p;
}

/**
 * @brief
 * \~english Notes the size asked for in the split.  Out of line: it only runs
 *          when measuring.
 * \~spanish Anota el tamano pedido en el reparto.  Fuera de linea: solo corre
 *          midiendo.
 * \~
 *
 * \~english
 * @par Threads
 * **NOT safe**, the same conditions as @c pop_block: it writes into @p c.
 *
 * \~spanish
 * @par Hilos
 * **NO segura**, mismas condiciones que @c pop_block: escribe en @p c.
 *
 * \~
 * @param c
 * \~english the cache whose histogram gets the mark.
 * \~spanish el cache en cuyo histograma se anota.
 * \~
 * @param n
 * \~english the size asked for, in bytes.
 * \~spanish el tamano pedido, en bytes.
 * \~
 */
void record_size(ThreadCache *c, size_t n) noexcept;

/**
 * @brief
 * \~english Registers this thread's cache if it does not have one yet.
 * \~spanish Da de alta el cache de este hilo si aun no lo tiene.
 * \~
 *
 * \~english
 * @c AllocScope needs it: a scope opened before the thread's first allocation
 * would have nowhere to leave the tag, and that whole phase would count as
 * "unknown" -- which is worse than not measuring, because it looks like data.
 *
 * Out of line because it happens once per thread.
 *
 * @par Threads
 * Safe.  The id is handed out with a `fetch_add`.
 *
 * \~spanish
 * Lo necesita @c AllocScope: un ambito que se abre antes de la primera reserva
 * del hilo no tendria donde dejar la etiqueta, y esa fase entera contaria como
 * "no se" -- que es peor que no medir, porque parece un dato.
 *
 * Fuera de linea porque ocurre una vez por hilo.
 *
 * @par Hilos
 * Segura.  El identificador se reparte con un `fetch_add`.
 *
 * \~
 * @return
 * \~english this thread's cache, or nullptr if the allocator is off or there
 *           are no thread slots left.
 * \~spanish el cache de este hilo, o nullptr si el asignador esta apagado o si
 *           ya no quedan ranuras de hilo.
 * \~
 */
ThreadCache *ensure_cache() noexcept;

/**
 * @brief
 * \~english Everything that is not taking a block off a list that already had
 *          one: registering the thread, the allocator being off, a large
 *          allocation, and the shared cache.
 * \~spanish Todo lo que no es sacar un bloque de una lista que ya lo tenia:
 *          alta del hilo, asignador apagado, reserva grande, y el cache
 *          compartido.
 * \~
 *
 * \~english
 * @par Threads
 * **Safe from any thread**; it works out on its own which cache to use.
 *
 * \~spanish
 * @par Hilos
 * **Segura desde cualquier hilo**; resuelve por su cuenta que cache usar.
 *
 * \~
 * @param n
 * \~english the size asked for, in bytes.
 * \~spanish el tamano pedido, en bytes.
 * \~
 * @return
 * \~english the block, or nullptr if there was no memory.
 * \~spanish el bloque, o nullptr si no habia memoria.
 * \~
 */
void *host_alloc_slow(size_t n) noexcept;

/**
 * @brief
 * \~english That class's list ran empty: collect the remote frees or ask for a
 *          chunk.
 * \~spanish La lista de esa clase se quedo vacia: recoger lo remoto o pedir un
 *          trozo.
 * \~
 *
 * \~english
 * @par Threads
 * **NOT safe** with respect to @p c: the same conditions as @c pop_block.  What
 * IS safe is where it gets the blocks from (the atomic stack and the region).
 *
 * \~spanish
 * @par Hilos
 * **NO segura** respecto a @p c: mismas condiciones que @c pop_block.  Lo que
 * SI es seguro es de donde saca los bloques (pila atomica y region).
 *
 * \~
 * @param c
 * \~english the cache to refill.
 * \~spanish el cache que hay que rellenar.
 * \~
 * @param k
 * \~english the size class that ran empty.
 * \~spanish la clase de tamano que se quedo vacia.
 * \~
 * @param n
 * \~english the size originally asked for, for the counters.
 * \~spanish el tamano que se pidio, para los contadores.
 * \~
 * @return
 * \~english a block of that class, or nullptr if there was no memory.
 * \~spanish un bloque de esa clase, o nullptr si no habia memoria.
 * \~
 */
void *host_alloc_refill(ThreadCache *c, uint32_t k, size_t n) noexcept;

/**
 * @brief
 * \~english Freeing a block that is not this thread's: onto its owner's atomic
 *          stack.
 * \~spanish Liberar un bloque que no es de este hilo: a la pila atomica de su
 *          dueño.
 * \~
 *
 * \~english
 * @par Threads
 * **Safe from any thread**, with a `compare_exchange` and blocking nobody.
 *
 * \~spanish
 * @par Hilos
 * **Segura desde cualquier hilo**, con un `compare_exchange` y sin bloquear.
 *
 * \~
 * @param p
 * \~english the block to give back.
 * \~spanish el bloque a devolver.
 * \~
 * @param h
 * \~english its chunk header, which is where the owner is read from.
 * \~spanish la cabecera de su trozo, que es de donde se lee el dueño.
 * \~
 */
void host_free_remote(void *p, ChunkHeader *h) noexcept;

/**
 * @brief
 * \~english Freeing something that is NOT a size-class block: a large span, or
 *          rubbish.
 * \~spanish Liberar algo que NO es un bloque de una clase: un tramo grande, o
 *          basura.
 * \~
 *
 * \~english
 * @par Threads
 * **Safe from any thread**; it takes the span lock if it has to.
 *
 * \~spanish
 * @par Hilos
 * **Segura desde cualquier hilo**; toma el cerrojo de tramos si hace falta.
 *
 * \~
 * @param p
 * \~english the pointer that was handed to @c host_free.
 * \~spanish el puntero que se le paso a @c host_free.
 * \~
 * @param h
 * \~english its chunk header, already located.
 * \~spanish la cabecera de su trozo, ya localizada.
 * \~
 */
void host_free_not_small(void *p, ChunkHeader *h) noexcept;

/**
 * @brief
 * \~english Frees a block from the region of the BIG classes.
 * \~spanish Suelta un bloque de la region de las clases GRANDES.
 * \~
 *
 * \~english
 * Out of line because they are 0.9% of the frees: putting it in the header
 * would fatten the hot path to serve the rare case.
 *
 * \~spanish
 * Fuera de linea porque son el 0,9% de las liberaciones: meterlo en la cabecera
 * engordaria el camino caliente para servir al caso raro.
 *
 * \~
 * @param p
 * \~english the block to give back.
 * \~spanish el bloque a devolver.
 * \~
 */
void host_free_big(void *p) noexcept;

/// \~english Thread slot of the lock-free per-thread policy.  Separate from
///           @c g_cache_slot because a thread may use both allocators at once.
/// \~spanish La ranura por hilo de la politica sin cerrojos.  Aparte de
///           @c g_cache_slot porque un hilo puede usar los dos asignadores a la
///           vez.
/// \~
extern ThreadSlot g_per_thread_slot;

/**
 * @brief
 * \~english Registers this thread with the lock-free policy and hands it its
 *          own cache.
 * \~spanish Da de alta este hilo en la politica sin cerrojos y le entrega su
 *          propio cache.
 * \~
 *
 * \~english
 * Cold: runs ONCE per thread.
 *
 * \~spanish
 * Fria: corre UNA vez por hilo.
 *
 * \~
 * @return
 * \~english the new cache, or nullptr only when that policy's id pool is
 *           exhausted -- there is no shared fallback here, by design.
 * \~spanish el cache nuevo, o nullptr solo cuando se agotan los
 *           identificadores de esa politica -- aqui no hay respaldo
 *           compartido, por diseño.
 * \~
 */
ThreadCache *per_thread_cache_slow() noexcept;

/**
 * @brief
 * \~english This thread's cache under the lock-free policy, or nullptr the
 *          first time.
 * \~spanish El cache de este hilo en la politica sin cerrojos, o nullptr la
 *          primera vez.
 * \~
 *
 * \~english
 * @par Threads
 * Safe: a thread slot is private to the thread that reads it.
 *
 * \~spanish
 * @par Hilos
 * Segura: una ranura por hilo es privada del hilo que la lee.
 *
 * \~
 * @return
 * \~english the cache, or nullptr while the thread has not been registered.
 * \~spanish el cache, o nullptr mientras el hilo no se haya dado de alta.
 * \~
 */
[[gnu::always_inline]] inline ThreadCache *per_thread_cache() noexcept {
    return static_cast<ThreadCache *>(g_per_thread_slot.get());
}

/**
 * @brief
 * \~english Stops the process: a block that is not ours has reached the point
 *          of being freed.
 * \~spanish Para el proceso: ha llegado a soltarse un bloque que no es nuestro.
 * \~
 *
 * \~english
 * IT DOES NOT RETURN.  It is the other half of having no fallback when
 * allocating: if nobody falls back to the system when asking, there can be no
 * system blocks to free, and one showing up is proof that somebody allocated
 * through a door we are not watching.  Handing it to `free` would work -- and
 * that is exactly the trap: the program would carry on, slower, and the
 * allocator's figures would stop describing what happens without anything
 * failing.
 *
 * Out of line and cold: the hot free path never sees it.
 *
 * \~spanish
 * NO DEVUELVE.  Es la otra mitad de no tener respaldo al reservar: si nadie se
 * cae al sistema pidiendo, no puede haber bloques del sistema que soltar, y uno
 * que aparezca es la prueba de que alguien reservo por una puerta que no
 * miramos.  Pasarselo a `free` funcionaria -- y esa es justamente la trampa:
 * el programa seguiria, mas lento, y las cifras del asignador dejarian de
 * describir lo que pasa sin que nada fallara.
 *
 * Fuera de linea y fria: el camino caliente de soltar no la ve.
 *
 * \~
 * @param p
 * \~english the foreign pointer, named in the message so that it can be
 *           located.
 * \~spanish el puntero ajeno, que sale en el mensaje para poder localizarlo.
 * \~
 */
[[noreturn]] void no_foreign_free(void *p) noexcept;

/**
 * @brief
 * \~english Frees a pointer that falls outside BOTH regions.
 * \~spanish Suelta un puntero que cae fuera de las DOS regiones.
 * \~
 *
 * \~english
 * There are exactly two things it can be, and the table says which: a block
 * the system served on a reservation of its own -- everything above
 * @c kMaxSpanBytes -- or something that is not ours at all, which stops the
 * process through @c no_foreign_free.  The block is never READ to decide: a
 * pointer that might be foreign must not be dereferenced.
 *
 * Out of line and cold.  It sits on the branch that used to end the program,
 * so recognising the direct blocks costs the hot free path nothing --
 * VERIFIED BY DISASSEMBLY: @c host_free comes out instruction for instruction
 * as it was down the in-region path, and the three instructions this adds are
 * all on that dead branch, which now ends in a tail call instead of a call to
 * something that never returned.
 *
 * \~spanish
 * Solo puede ser una de dos cosas, y la tabla dice cual: un bloque que el
 * sistema sirvio en una reserva propia -- todo lo que pasa de
 * @c kMaxSpanBytes -- o algo que no es nuestro, que para el proceso por
 * @c no_foreign_free.  Nunca se LEE el bloque para decidirlo: un puntero que
 * podria ser ajeno no se desreferencia.
 *
 * Fuera de linea y fria.  Vive en la rama que antes terminaba el programa, asi
 * que reconocer los bloques directos no le cuesta nada al camino caliente de
 * soltar -- COMPROBADO DESENSAMBLANDO: @c host_free sale instruccion por
 * instruccion como estaba por el camino de dentro de la region, y las tres
 * instrucciones que esto anade estan todas en esa rama muerta, que ahora
 * termina en una llamada de cola en vez de en una llamada a algo que no
 * volvia.
 *
 * \~
 * @param p
 * \~english the pointer that was handed to a free entry point.
 * \~spanish el puntero que se le paso a una entrada de liberacion.
 * \~
 */
void host_free_outside(void *p) noexcept;

/**
 * @brief
 * \~english How big a block the system served directly is, or 0 if @p p is not
 *          one.
 * \~spanish Cuanto mide un bloque que sirvio el sistema directamente, o 0 si
 *          @p p no lo es.
 * \~
 *
 * \~english
 * It counts FROM @p p, not from the start of the block, which matters when the
 * caller was handed a pointer raised to an alignment inside it -- see
 * @c host_alloc_aligned_freeable.  Answering with the whole size there would
 * promise bytes that are behind the pointer.
 *
 * What it counts is the bytes ASKED OF THE SYSTEM, so the rounding up to a
 * page is included: it is real memory, not slack.
 *
 * \~spanish
 * Cuenta DESDE @p p, no desde el principio del bloque, que es lo que importa
 * cuando al llamante se le entrego un puntero subido a una alineacion dentro
 * de el -- ver @c host_alloc_aligned_freeable --.  Contestar ahi con el tamano
 * entero prometeria bytes que estan por detras del puntero.
 *
 * Lo que cuenta son los bytes que se le PIDIERON AL SISTEMA, asi que el
 * redondeo a pagina entra: es memoria de verdad, no holgura.
 *
 * \~
 * @param p
 * \~english the pointer to place; anywhere INSIDE the block will do.
 * \~spanish el puntero que hay que situar; vale cualquiera de DENTRO del
 *          bloque.
 * \~
 * @return
 * \~english the room left from it, or 0 when it is in no block of ours.
 * \~spanish el sitio que queda desde el, o 0 cuando no esta en ningun bloque
 *          nuestro.
 * \~
 */
size_t direct_bytes(const void *p) noexcept;

} // namespace detail

/**
 * @brief
 * \~english Allocates @p n bytes, aligned as `operator new` guarantees.
 * \~spanish Sirve @p n bytes con la garantia de alineacion de `operator new`.
 * \~
 *
 * \~english
 * The everyday entry point.  Small sizes come from a free list belonging to
 * the calling thread and cost a handful of instructions; larger ones go to a
 * span, and larger still to the operating system.
 *
 * \~spanish
 * La entrada de todos los dias.  Los tamanos pequenos salen de una lista de
 * libres del propio hilo y cuestan un punado de instrucciones; los mayores van
 * a un tramo, y los muy grandes al sistema operativo.
 *
 * \~
 * @param n
 * \~english bytes wanted.  Zero is accepted and returns a usable block, so
 *           callers do not need a special case for it.
 * \~spanish bytes que se quieren.  El cero se acepta y devuelve un bloque
 *           utilizable, para que quien llama no tenga que tratarlo aparte.
 * \~
 * @return
 * \~english the block, or nullptr when there is no memory left.
 * \~spanish el bloque, o nullptr cuando ya no hay memoria.
 * \~
 *
 * @par Threads
 * \~english **Safe from any thread**, and it synchronises nothing on the
 * normal path.  That is not a contradiction: the safety comes from PARTITIONING
 * the state -- each thread has its own lists -- rather than from protecting a
 * shared one.  The little that really is shared sits off the fast path: the
 * stack of blocks freed by other threads (one `compare_exchange`), the span
 * lists and the overflow cache (a lock, and one operation in a hundred
 * thousand).
 * \~spanish **Segura desde cualquier hilo**, y sin sincronizar nada en el
 * camino normal.  No es una contradiccion: la seguridad sale de PARTIR el
 * estado -- cada hilo tiene sus listas --, no de proteger uno compartido.  Lo
 * poco que de verdad se comparte esta fuera del camino rapido: la pila de
 * liberaciones ajenas (con un `compare_exchange`), las listas de tramos y el
 * cache de desbordamiento (con cerrojo, y son una de cada cien mil
 * operaciones).
 * \~
 *
 * \~english
 * @code
 * void *p = util::host_alloc(64);
 * if (p == nullptr) return;          // out of memory
 * util::host_free(p);
 * @endcode
 *
 * \~spanish
 * @code
 * void *p = util::host_alloc(64);
 * if (p == nullptr) return;          // sin memoria
 * util::host_free(p);
 * @endcode
 *
 * \~
 *
 * @see host_free, host_alloc_zeroed, host_alloc_aligned
 */
[[gnu::always_inline]] inline void *host_alloc(size_t n) noexcept {
    /* Un solo salto cubre los dos casos raros: con `n == 0` la resta da el
     * mayor sin signo, que tambien cae fuera.  Asi el tamano se valida sin
     * gastar una segunda comparacion en algo que no pasa casi nunca. */
    if (n - 1 >= kMaxSmall) return detail::host_alloc_slow(n);
    detail::ThreadCache *c = detail::current_cache();
    /* One unsigned comparison for "no cache" and "this thread is already being
     * torn down"; see @c kDyingCache.  It is the same single instruction the
     * null test compiled to. */
    if (!detail::have_cache(c)) return detail::host_alloc_slow(n);
#if VESTA_ALLOC_SIZE_HISTOGRAM
    /* El portillo del histograma de tamanos.  Con la medida apagada -- que es
     * lo normal en produccion -- son una carga de un global y un salto por
     * reserva para no hacer nada.  Compilarlo fuera es la unica forma de que no
     * se paguen; ver `VESTA_ALLOC_SIZE_HISTOGRAM`. */
    if (detail::g_measure) detail::record_size(c, n);
#endif
    const uint32_t k = class_of(n);
    void *p = detail::pop_block(c, k);
    if (p == nullptr) return detail::host_alloc_refill(c, k, n);
    return p;
}

/**
 * @brief
 * \~english Allocates @p n bytes aligned to @p align, from THIS allocator.
 * \~spanish Sirve @p n bytes alineados a @p align, con ESTE asignador.
 * \~
 *
 * \~english
 * @c host_alloc only guarantees what `operator new` does -- 16 bytes -- so a
 * type with `alignas(32)` or more cannot use it.  Without this the only way out
 * was the SYSTEM allocator, and then the process ends up with two allocators at
 * once: one object allocated through one path and released through the other is
 * enough to corrupt the heap, and the symptom is not an error but a
 * segmentation fault somewhere else and much later.
 *
 * HOW: more is asked for, the pointer is pushed up to the alignment wanted, and
 * the original is kept in the gap just before it -- which is all that is needed
 * to release it later.
 *
 * \~spanish
 * @c host_alloc solo garantiza la alineacion natural de `operator new` -- 16
 * bytes --, asi que un tipo con `alignas(32)` o mas no lo puede usar.  Sin esto
 * la unica salida era el asignador del SISTEMA, y entonces el proceso acaba con
 * dos asignadores a la vez: basta con que un objeto se reserve por un camino y
 * se suelte por el otro para corromper el monton, y el sintoma no es un error
 * sino una violacion de segmento en otro sitio y mucho despues.
 *
 * COMO: se pide de mas, se sube el puntero hasta la alineacion pedida y el
 * original se guarda en el hueco de justo antes, que es lo unico que hace falta
 * para poder soltarlo.
 *
 * \~
 * \~english
 * @code
 * struct alignas(64) Line { char data[64]; };
 * void *p = util::host_alloc_aligned(sizeof(Line), alignof(Line));
 * util::host_free_aligned(p);        // NOT host_free: they come in pairs
 * @endcode
 *
 * \~spanish
 * @code
 * struct alignas(64) Line { char data[64]; };
 * void *p = util::host_alloc_aligned(sizeof(Line), alignof(Line));
 * util::host_free_aligned(p);        // NO host_free: van emparejadas
 * @endcode
 *
 * \~
 * \~spanish
 * LO QUE SE PIDE DE MAS SON @p align BYTES, no @p align - 1 + 8, y la
 * diferencia no es cosmetica: los tamanos se redondean a una CLASE, asi que
 * siete bytes de mas pueden costar una clase entera.  Un `alignas(64)` de 64
 * bytes pedia 135 y caia en la clase de 160; pidiendo 128 cae en la de 128.
 *
 * Se puede pedir menos porque @c host_alloc ya entrega 16 alineados en TODOS
 * sus caminos -- los bloques de un trozo salen a `trozo + 16` con paso multiplo
 * de 16, un tramo sale tambien a `trozo + 16`, y el respaldo del sistema
 * garantiza `max_align_t` --.  Con el original ya 16 alineado, la distancia
 * hasta el siguiente multiplo de @p align ESTRICTAMENTE mayor que el es como
 * mucho @p align, y como poco 16: lo primero acota lo que hay que pedir, y lo
 * segundo garantiza que los 8 bytes de la cabecera caben siempre.
 *
 * \~english
 * WHAT IS ASKED FOR ON TOP IS @p align BYTES, not @p align - 1 + 8, and the
 * difference is not cosmetic: sizes are rounded up to a CLASS, so seven extra
 * bytes can cost a whole class.  An `alignas(64)` of 64 bytes used to ask for
 * 135 and land in the 160 class; asking for 128 lands in the 128 one.
 *
 * Less can be asked for because @c host_alloc already hands back 16-aligned
 * memory on ALL of its paths.  With the original already 16-aligned, the
 * distance to the next multiple of @p align STRICTLY greater than it is at most
 * @p align and at least 16: the first bounds what has to be asked for, the
 * second guarantees the eight header bytes always fit.
 *
 * \~
 * @param n
 * \~english useful bytes.
 * \~spanish bytes utiles.
 * \~
 * @param align
 * \~english the alignment wanted.  A power of two.
 * \~spanish la alineacion pedida.  Potencia de dos.
 * \~
 * @return
 * \~english the aligned block, or nullptr if there is no memory.
 * \~spanish el bloque alineado, o nullptr si no hay memoria.
 * \~
 *
 * @par Threads
 * \~english **Safe from any thread**, the same as @c host_alloc: it adds no
 * state.
 * \~spanish **Segura desde cualquier hilo**, la misma que @c host_alloc: no
 * anade estado.
 * \~
 *
 * @see host_free_aligned, host_alloc_aligned_freeable
 */
[[gnu::always_inline]] inline void *host_alloc_aligned(size_t n,
                                                       size_t align) noexcept {
    /* El paso no baja de la alineacion natural del asignador.  Por debajo de
     * 16, la distancia hasta el destino podria quedarse en menos de los 8 bytes
     * que necesita la cabecera y se escribiria ANTES del bloque.  Antes esto no
     * hacia falta y habia una nota diciendo que no se ponia "por si acaso"; con
     * el relleno justo si hace falta, y ademas esta funcion es publica: dar por
     * hecho que solo la llama `operator new` es dar por hecho algo que el
     * compilador no puede comprobar.  Es un `cmov`, y en un camino que solo
     * pisan los tipos sobre-alineados. */
    const size_t step = align < kAlign ? kAlign : align;
    if (__builtin_expect(n > (size_t)-1 - step, 0)) return nullptr;
    void *raw = host_alloc(n + step);
    if (__builtin_expect(raw == nullptr, 0)) return nullptr;
    /* `+ step` antes de truncar es lo que fuerza a subir SIEMPRE al menos una
     * posicion: si `raw` ya estuviera alineado, quedarse donde esta no dejaria
     * sitio para la cabecera. */
    const uintptr_t aligned =
        ((uintptr_t)raw + step) & ~(uintptr_t)(align - 1u);
    ((void **)aligned)[-1] = raw; // el original, para poder soltarlo
    return (void *)aligned;
}

/**
 * @brief
 * \~english Returns a block that came from @c host_alloc.
 * \~spanish Devuelve un bloque de @c host_alloc.
 * \~
 *
 * \~english
 * It works even if ANOTHER thread allocated it: that is the case that sinks
 * naive per-thread allocators, and here it is solved with no locks and no
 * tables.
 *
 * \~spanish
 * Vale aunque lo reservara OTRO hilo: es el caso que hunde a los asignadores
 * por hilo ingenuos, y aqui esta resuelto sin cerrojos ni tablas.
 *
 * \~
 * @param p
 * \~english the block, or nullptr, which does nothing.  It must have come from
 *           @c host_alloc: a pointer this allocator never handed out is a hard
 *           error, not a case to tolerate.
 * \~spanish el bloque, o nullptr, que no hace nada.  Tiene que venir de
 *           @c host_alloc: un puntero que este asignador no entrego es un error
 *           duro, no un caso a tolerar.
 * \~
 *
 * @par Threads
 * \~english **Safe from any thread.**  If the block belongs to this thread it
 * goes to its list without synchronising anything.  If it belongs to another,
 * it goes to that owner's atomic stack with a `compare_exchange`, blocking
 * nobody.  Freeing the same block twice is NOT safe -- as in any allocator --
 * but it is at least DETECTED and reported instead of corrupting in silence.
 * \~spanish **Segura desde cualquier hilo.**  Si el bloque es de este hilo, va
 * a su lista sin sincronizar nada.  Si es de otro, va a la pila atomica de su
 * dueno con un `compare_exchange`, sin bloquear a nadie.  Liberar dos veces el
 * mismo bloque NO es seguro -- como en cualquier asignador --, pero al menos se
 * DETECTA y se avisa en vez de corromper en silencio.
 * \~
 *
 * \~english
 * @code
 * void *p = util::host_alloc(1024);
 * // another thread may free it, and that is supported
 * util::host_free(p);
 * @endcode
 *
 * \~spanish
 * @code
 * void *p = util::host_alloc(1024);
 * // lo puede soltar otro hilo, y esta soportado
 * util::host_free(p);
 * @endcode
 *
 * \~
 *
 * @see host_alloc, host_free_aligned
 */
[[gnu::always_inline]] inline void host_free(void *p) noexcept {
    if (p == nullptr) return;
    if (!in_region(p)) {
        /* Aqui no llega NUNCA un bloque pequeno, y por eso la region de las
         * clases grandes se pregunta justo aqui y no antes: el camino que si es
         * caliente sigue siendo el mismo que antes de que existiera, sin una
         * comparacion de mas.  Comprobado desensamblando. */
        if (in_big_region(p)) {
            detail::host_free_big(p);
            return;
        }
        /* FUERA DE LAS DOS REGIONES.  O es un bloque que el sistema sirvio en
         * una reserva propia -- todo lo que pasa de `kMaxSpanBytes` -- o no es
         * nuestro, y entonces se para el proceso: si nadie cae al sistema al
         * reservar, un bloque ajeno aqui significa que alguien reservo por otra
         * puerta, y devolverselo a `free` lo taparia.  Ver `no_fallback`. */
        // std::free(p);
        detail::host_free_outside(p);
        return;
    }
    ChunkHeader *h = chunk_of(p);
    if (h->magic != kChunkMagic) {
        // Un tramo (reserva grande), o algo que no deberia estar aqui.  Las dos
        // cosas fuera de linea: ninguna es el caso normal.
        detail::host_free_not_small(p, h);
        return;
    }
    detail::ThreadCache *c = detail::current_cache();
    if (detail::have_cache(c) && h->owner == c->id) {
        detail::push_block(c, p, h->cls);
        return;
    }
    detail::host_free_remote(p, h);
}

/**
 * @brief
 * \~english Returns a block that came from @c host_alloc_aligned.
 * \~spanish Devuelve un bloque de @c host_alloc_aligned.
 * \~
 *
 * \~english
 * It does NOT work for blocks from @c host_alloc, nor the other way round: the
 * pointer handed back is not the one that was allocated, and the original lives
 * in the gap just before it.
 *
 * \~spanish
 * NO vale para bloques de @c host_alloc, ni al reves: el puntero que se
 * devolvio no es el que se reservo, y el original vive en el hueco de justo
 * antes.
 *
 * \~
 * @param p
 * \~english a block from @c host_alloc_aligned, or nullptr, which does nothing.
 * \~spanish un bloque de @c host_alloc_aligned, o nullptr, que no hace nada.
 * \~
 *
 * @par Threads
 * \~english **Safe from any thread**, the same as @c host_free.
 * \~spanish **Segura desde cualquier hilo**, la misma que @c host_free.
 * \~
 *
 * @see host_alloc_aligned
 */
[[gnu::always_inline]] inline void host_free_aligned(void *p) noexcept {
    if (p == nullptr) return;
    host_free(((void **)p)[-1]);
}

/**
 * @brief
 * \~english An aligned block that the ORDINARY @c host_free releases.
 * \~spanish Un bloque alineado que suelta el @c host_free NORMAL.
 * \~
 *
 * \~english
 * WHY A SECOND ALIGNED ENTRY.  Because @c host_alloc_aligned asks the caller to
 * remember: it hands back a pointer that is not the one it reserved, so only
 * @c host_free_aligned can undo it.  That is a fine contract when the two ends
 * are written together -- `operator new(align_val_t)` and its `delete` are --
 * and it is not available at all in C, where `posix_memalign` and
 * `aligned_alloc` are released with plain `free`.  There the block has to say
 * what it is BY ITSELF.
 *
 * HOW, WITHOUT MARKING ANYTHING.  A magic word in front of the pointer would be
 * a guess against whatever the previous block left there, and a side table
 * would tax every free in the program to serve the rare one.  Neither is
 * needed, because the allocator already answers this question in two different
 * ways and one of them fits:
 *
 *   - a CLASS block is identified by its own address, so the pointer has to be
 *     the start of the block -- an offset pointer is unrecognisable;
 *   - a SPAN is identified by MASKING to its first chunk, and `free_span` takes
 *     the header alone: it never looks at the pointer.
 *
 * So an over-aligned block is served as a span with the user pointer pushed up
 * inside the first chunk.  @c host_free finds `kSpanMagic` by the route it
 * already takes, and not one instruction of the free path changes.
 *
 * WHAT IT COSTS, AND TO WHOM.  Nothing to anybody who does not call it.  To the
 * caller, an over-aligned request takes a whole chunk (@c kChunkBytes) at
 * least, even for sixteen bytes -- that is the price of being releasable
 * without being remembered.  An alignment the allocator already gives (16 or
 * under) costs nothing at all: it goes straight to @c host_alloc.
 *
 * WHY THERE IS A CEILING, AND WHY IT IS STRICT.  A span is chunk-granular and
 * its header lives in the FIRST chunk, so the aligned address has to stay
 * inside that chunk for the masking to find it.  Rounding `chunk + 16` up lands
 * on `chunk + align`, which is still inside only while `align < kChunkBytes`:
 * at exactly @c kChunkBytes it is the base of the NEXT chunk, which has no
 * header of its own, and @c host_free would land on the corruption path.  That
 * off-by-one boundary is not hypothetical -- it is what
 * `test_malloc_interpose` caught the first time this was written with `<=`.
 * Serving it would be worse than not serving it, so it says no and the caller
 * can tell.
 *
 * @par Threads
 * **Safe from any thread**, the same as @c host_alloc: it adds no state.
 *
 * \~spanish
 * POR QUE UNA SEGUNDA ENTRADA ALINEADA.  Porque @c host_alloc_aligned le pide a
 * quien llama que se acuerde: devuelve un puntero que no es el que reservo, asi
 * que solo @c host_free_aligned puede deshacerlo.  Es un contrato aceptable
 * cuando los dos extremos se escriben juntos -- `operator new(align_val_t)` y
 * su `delete` lo estan -- y no existe en C, donde `posix_memalign` y
 * `aligned_alloc` se sueltan con el `free` de siempre.  Ahi el bloque tiene que
 * decir lo que es POR SI SOLO.
 *
 * COMO, SIN MARCAR NADA.  Una palabra magica delante del puntero seria una
 * apuesta contra lo que dejara ahi el bloque anterior, y una tabla aparte le
 * cobraria a todas las liberaciones del programa para servir a la rara.  No
 * hace falta ninguna de las dos, porque el asignador ya contesta a esta
 * pregunta de dos formas distintas y una de ellas encaja:
 *
 *   - un bloque de CLASE se identifica por su propia direccion, asi que el
 *     puntero tiene que ser el principio del bloque -- uno desplazado no se
 *     reconoce --;
 *   - un TRAMO se identifica ENMASCARANDO hasta su primer trozo, y `free_span`
 *     se queda con la cabecera: no mira el puntero en ningun momento.
 *
 * Asi que un bloque sobrealineado se sirve como tramo con el puntero del
 * usuario subido dentro del primer trozo.  @c host_free encuentra `kSpanMagic`
 * por el camino que ya recorre, y no cambia ni una instruccion de liberar.
 *
 * QUE CUESTA, Y A QUIEN.  Nada a quien no la llame.  A quien la llama, una
 * peticion sobrealineada se lleva un trozo entero (@c kChunkBytes) como minimo,
 * aunque sean dieciseis bytes -- ese es el precio de poder soltarse sin que
 * nadie se acuerde --.  Una alineacion que el asignador ya da (16 o menos) no
 * cuesta nada en absoluto: se va derecha a @c host_alloc.
 *
 * POR QUE HAY UN TECHO, Y POR QUE ES ESTRICTO.  Un tramo va por trozos y su
 * cabecera vive en el PRIMERO, asi que la direccion alineada tiene que quedarse
 * dentro de ese trozo para que la mascara la encuentre.  Redondear
 * `trozo + 16` cae en `trozo + align`, que sigue dentro solo mientras
 * `align < kChunkBytes`: justo en @c kChunkBytes es la base del trozo
 * SIGUIENTE, que no tiene cabecera propia, y @c host_free acabaria en el camino
 * de corrupcion.  Esa frontera de uno no es hipotetica -- es lo que cazo
 * `test_malloc_interpose` la primera vez que esto se escribio con `<=` --.
 * Servirla seria peor que no servirla, asi que dice que no y quien llama se
 * entera.
 *
 * @par Hilos
 * **Segura desde cualquier hilo**, igual que @c host_alloc: no anade estado.
 *
 * \~
 * @param n
 * \~english useful bytes.
 * \~spanish bytes utiles.
 * \~
 * @param align
 * \~english the alignment asked for.  A power of two SMALLER than
 *           @c kChunkBytes.
 * \~spanish la alineacion pedida.  Potencia de dos MENOR que @c kChunkBytes.
 * \~
 * @return
 * \~english the aligned block, or nullptr -- out of memory, or an alignment
 *           this cannot serve.  A bigger one is REFUSED, never faked.
 * \~spanish el bloque alineado, o nullptr -- sin memoria, o una alineacion que
 *           esto no puede servir.  Una mayor se RECHAZA, nunca se finge.
 * \~
 *
 * \~english
 * @code
 *   // What `posix_memalign` needs: released with the ordinary free.
 *   void *p = util::host_alloc_aligned_freeable(n, 64);
 *   if (p == nullptr) return ENOMEM;
 *   ...
 *   util::host_free(p);              // no need to remember anything
 * @endcode
 *
 * \~spanish
 * @code
 *   // Lo que necesita `posix_memalign`: se suelta con el free de siempre.
 *   void *p = util::host_alloc_aligned_freeable(n, 64);
 *   if (p == nullptr) return ENOMEM;
 *   ...
 *   util::host_free(p);              // sin que nadie tenga que acordarse
 * @endcode
 *
 * \~
 */
[[gnu::always_inline]] inline void *host_alloc_aligned_freeable(
    size_t n, size_t align) noexcept {
    /* Everything this allocator hands out is already 16-aligned, on every one
     * of its paths.  So the whole cheap half of the range is just `host_alloc`,
     * which is also the common case: `max_align_t` is 16, and that is what a
     * portable `posix_memalign` asks for. */
    if (align <= kAlign) return host_alloc(n);
    if (__builtin_expect(align >= kChunkBytes, 0)) return nullptr;

    /* The request has to reach the SPAN path, and what routes it there is the
     * size.  Asking for `n + align` covers the bytes lost to the offset; if
     * that still fits in a class, it is raised until it does not -- a class
     * block could not be recognised from an offset pointer, which is the whole
     * point of this entry. */
    size_t want = n + align;
    if (__builtin_expect(n > (size_t)-1 - align, 0)) return nullptr;
    if (want <= kMaxSmall) want = kMaxSmall + 1;
    void *raw = host_alloc(want);
    if (__builtin_expect(raw == nullptr, 0)) return nullptr;
    /* A span starts at `chunk + sizeof(ChunkHeader)` and its chunk is aligned
     * to `kChunkBytes`, so rounding up lands on `chunk + align` and stays
     * inside the first chunk -- which is what makes the masking find the
     * header.  The bytes skipped are the ones asked for above.
     *
     * Past `kMaxSpanBytes` there is no header to find: the block came straight
     * from the system, and what places the raised pointer is the table, which
     * answers for anything INSIDE the block and not only for its base.  Both
     * ends of the promise -- `host_free` takes it, `host_usable_size` counts
     * from it -- hold the same way on either path. */
    return (void *)(((uintptr_t)raw + align - 1u) & ~(uintptr_t)(align - 1u));
}

/**
 * @brief
 * \~english Declares what this thread is allocating FOR, while it lives.
 * \~spanish Declara para que es lo que se reserve en este hilo mientras viva.
 * \~
 *
 * \~english
 * This is the AMBIENT channel for the tag, and it exists because some
 * allocations cannot declare anything by themselves: a `std::string` has
 * nowhere to say what it is for, and third-party code even less.  Wrapping a
 * phase tags everything allocated inside it without touching a single call
 * site.
 *
 * It nests: on the way out the previous tag is put back.
 *
 * IT APPLIES TO THIS THREAD ONLY.  If the phase farms work out to a pool, the
 * workers inherit nothing by themselves -- theirs would show up as "unknown",
 * which is a false figure rather than a missing one.  The tag travels with the
 * task.
 *
 * \~spanish
 * Es el canal AMBIENTAL de la etiqueta, y existe porque hay reservas que no
 * pueden declarar nada por si mismas: un `std::string` no tiene donde decir su
 * proposito, y el codigo de terceros menos aun.  Envolviendo una fase, todo lo
 * que reserve dentro queda etiquetado sin tocar un solo sitio de llamada.
 *
 * Anida: al salir se restaura la etiqueta que hubiera antes.
 *
 * SOLO VALE PARA ESTE HILO.  Si la fase reparte trabajo, los trabajadores no
 * heredan nada por su cuenta -- lo suyo apareceria como "no se", que es un dato
 * falso, no un dato que falta.  La etiqueta viaja con la tarea.
 *
 * \~
 * @par Threads
 * \~english Each object is for ITS thread and only for it.  Several threads can
 * have one at the same time without getting in each other's way -- each writes
 * to its own cache -- but one object CANNOT be shared: that is why it is not
 * copyable.
 * \~spanish Cada objeto vale para SU hilo y solo para el.  Varios hilos pueden
 * tener el suyo a la vez sin estorbarse -- cada uno escribe en su propio cache
 * --, pero un mismo objeto NO se puede compartir: por eso no es copiable.
 * \~
 *
 * \~english
 * @code
 *   util::AllocScope phase{{util::AllocUse::Medium, util::AllocShape::Growing}};
 *   // ...everything allocated in here is counted as that
 * @endcode
 *
 * \~spanish
 * @code
 *   util::AllocScope fase{{util::AllocUse::Medium, util::AllocShape::Growing}};
 *   // ...todo lo que reserve aqui dentro queda contado como tal
 * @endcode
 *
 * \~
 */
class AllocScope {
  public:
    /**
     * @brief
     * \~english Puts the tag in place and keeps the one that was there.
     * \~spanish Pone la etiqueta y guarda la que habia.
     * \~
     *
     * \~english
     * THE USUAL CASE IS INLINE.  A thread has a cache from its first allocation
     * on, so the normal shape is one slot read and two writes, with no call at
     * all.  `ensure_cache` is out of line and only paid the first time in each
     * thread.
     *
     * It matters more than it looks because this is no longer opened once per
     * phase: the thread pool builds one per TASK so that the tag travels with
     * the work that gets handed out.
     *
     * \~spanish
     * EL CASO DE SIEMPRE VA EN LINEA.  Un hilo tiene cache desde su primera
     * reserva, asi que lo normal es una lectura de la ranura y dos escrituras,
     * sin ninguna llamada.  `ensure_cache` esta fuera de linea y solo se paga
     * la primera vez de cada hilo.
     *
     * Importa mas de lo que parece porque esto ya no se abre solo una vez por
     * fase: el pool de hilos construye uno por TAREA para que la etiqueta viaje
     * con el trabajo repartido.
     *
     * \~
     * @param t
     * \~english what everything allocated inside the scope is counted as.
     * \~spanish como se cuenta todo lo que se reserve dentro del ambito.
     * \~
     */
    explicit AllocScope(AllocTag t) noexcept { open(t.raw(), kKeepFill); }

    /**
     * @brief
     * \~english The two purpose axes, without the double braces.
     * \~spanish Los dos ejes de proposito, sin las llaves dobles.
     * \~
     * @param u
     * \~english how long it lives.  \~spanish cuanto vive.  \~
     * @param s
     * \~english whether it grows.  \~spanish si crece.  \~
     */
    AllocScope(AllocUse u, AllocShape s) noexcept {
        open(AllocTag{u, s}.raw(), kKeepFill);
    }

    /**
     * @brief
     * \~english ALL THREE axes at once, which is how a phase declares itself.
     * \~spanish LOS TRES ejes de una vez, que es como se declara una fase.
     * \~
     * @param u
     * \~english how long it lives.  \~spanish cuanto vive.  \~
     * @param s
     * \~english whether it grows.  \~spanish si crece.  \~
     * @param f
     * \~english how much of it gets touched.  \~spanish cuanto se toca de ello.
     * \~
     */
    AllocScope(AllocUse u, AllocShape s, AllocFill f) noexcept {
        open(AllocTag{u, s}.raw(), static_cast<uint8_t>(f));
    }

    /// \~english The tag as one value, plus how much gets touched.
    /// \~spanish La etiqueta como un valor, mas cuanto se toca.  \~
    AllocScope(AllocTag t, AllocFill f) noexcept {
        open(t.raw(), static_cast<uint8_t>(f));
    }

    /**
     * @brief
     * \~english Only how much gets touched, leaving the purpose as it was.
     * \~spanish Solo cuanto se toca, dejando el proposito como estaba.
     * \~
     *
     * \~english
     * For a phase already inside another that declared the purpose: what
     * changes is what this bit of it does with the memory, not what it is for.
     *
     * \~spanish
     * Para un tramo que ya esta dentro de otro que declaro el proposito: lo que
     * cambia es lo que ESTE trozo hace con la memoria, no para que es.
     *
     * \~
     * @param f
     * \~english how much of it gets touched.  \~spanish cuanto se toca de ello.
     * \~
     */
    explicit AllocScope(AllocFill f) noexcept {
        open(kKeepTag, static_cast<uint8_t>(f));
    }

    /**
     * @brief
     * \~english Puts back what was in place before, on both axes.
     * \~spanish Vuelve a poner lo que habia antes, en los dos ejes.
     * \~
     *
     * \~english
     * BOTH ARE ALWAYS RESTORED, even the one this scope did not set: it saved
     * what was there and puts the same thing back, so nothing changes and there
     * is no "did I touch this one" flag to get wrong.
     *
     * \~spanish
     * SE RESTAURAN SIEMPRE LOS DOS, incluso el que este ambito no puso: guardo
     * lo que hubiera y devuelve lo mismo, asi que no cambia nada y no hay
     * ninguna marca de "toque este" que se pueda equivocar.
     *
     * \~
     */
    ~AllocScope() noexcept {
        if (c_ != nullptr) {
            c_->tag = prev_;
            c_->fill = prev_fill_;
        }
    }

    AllocScope(const AllocScope &) = delete;
    AllocScope &operator=(const AllocScope &) = delete;

    /**
     * @brief
     * \~english The tag running in this thread.
     * \~spanish La etiqueta que corre en este hilo.
     * \~
     * @return
     * \~english what is in place, or "unknown" while the thread has no cache.
     * \~spanish la que este puesta, o "no se" mientras el hilo no tenga cache.
     * \~
     */
    static AllocTag current() noexcept {
        const detail::ThreadCache *c = detail::current_cache();
        return detail::have_cache(c) ? AllocTag::from_raw(c->tag) : AllocTag{};
    }

    /**
     * @brief
     * \~english How much this thread is declaring it will touch.
     * \~spanish Cuanto esta declarando este hilo que va a tocar.
     * \~
     * @return
     * \~english what is in place, or "unknown" while the thread has no cache.
     * \~spanish lo que este puesto, o "no se" mientras el hilo no tenga cache.
     * \~
     */
    static AllocFill current_fill() noexcept {
        const detail::ThreadCache *c = detail::current_cache();
        return detail::have_cache(c) ? static_cast<AllocFill>(c->fill)
                                     : AllocFill::Unknown;
    }

  private:
    /// \~english "leave this axis as it is"  \~spanish "deja este eje como esta"
    /// \~
    static constexpr uint8_t kKeepTag = 0xFF;
    static constexpr uint8_t kKeepFill = 0xFF;

    /**
     * @brief
     * \~english The one place a scope is opened, whichever constructor was
     *          used.
     * \~spanish El unico sitio donde se abre un ambito, sea cual sea el
     *          constructor.
     * \~
     *
     * \~english
     * WRITTEN ONCE on purpose: five constructors each doing this by hand is
     * five chances for one of them to forget the dying-thread case, and that
     * one does not fail where it is written.
     *
     * A thread past its exit notice gets no cache: asking for one would take an
     * owner id that can never be given back -- see @c kDyingCache.  @c c_ is
     * left null so the destructor has nothing to put back either.
     *
     * \~spanish
     * ESCRITO UNA VEZ a proposito: cinco constructores haciendo esto a mano son
     * cinco ocasiones de que uno se olvide del caso del hilo que se muere, y
     * ese no falla donde esta escrito.
     *
     * Un hilo pasado su aviso de fin no tiene cache: pedirla tomaria un
     * identificador que ya no puede volver -- ver @c kDyingCache --.  @c c_ se
     * queda nulo para que el destructor tampoco tenga nada que devolver.
     *
     * \~
     */
    void open(uint8_t tag, uint8_t fill) noexcept {
        c_ = detail::current_cache();
        if (__builtin_expect(c_ == nullptr, 0)) c_ = detail::ensure_cache();
        if (__builtin_expect(!detail::have_cache(c_), 0)) {
            c_ = nullptr;
            return;
        }
        prev_ = c_->tag;
        prev_fill_ = c_->fill;
        if (tag != kKeepTag) c_->tag = tag;
        if (fill != kKeepFill) c_->fill = fill;
    }

    detail::ThreadCache *c_ = nullptr;
    uint8_t prev_ = 0;
    uint8_t prev_fill_ = 0;
};


/**
 * @brief
 * \~english Adds up the counters of every thread.
 * \~spanish Suma los contadores de todos los hilos.
 * \~
 *
 * @return
 * \~english a copy of the totals, taken at the moment of the call.
 * \~spanish una copia de los totales, tomada en el momento de la llamada.
 * \~
 *
 * @par Threads
 * \~english Callable from any thread, but **the snapshot is not consistent**:
 * the caches are walked without stopping them, so a counter can change while it
 * is being added up.  That is deliberate -- synchronising here would cost on
 * the fast path, and what these figures are for is orders of magnitude.  For an
 * exact number, read them when no thread is working any more.
 * \~spanish Se puede llamar desde cualquier hilo, pero **la foto no es
 * coherente**: se recorren los caches sin pararlos, asi que un contador puede
 * cambiar mientras se suma.  Es a proposito -- sincronizar aqui costaria en el
 * camino rapido, y lo que se quiere de estas cifras son ordenes de magnitud --.
 * Para un numero exacto, mirarlas cuando ya no queden hilos trabajando.
 * \~
 *
 * \~english
 * @code
 *   // How many allocations are still live, and how much was committed.
 *   const util::HostAllocStats s = util::host_alloc_stats();
 *   const long long live = (long long)(s.small_allocs + s.large_allocs) -
 *                          (long long)(s.small_frees + s.remote_frees +
 *                                      s.large_frees);
 *   std::printf("%lld live, %.1f MiB committed\n", live,
 *               s.bytes_reserved / (1024.0 * 1024.0));
 *
 *   // And how much of that never declared a purpose.
 *   const uint64_t undeclared = s.by_tag[util::AllocTag{}.raw()];
 * @endcode
 *
 * \~spanish
 * @code
 *   // Cuantas reservas quedaron vivas, y cuanto se comprometio por ellas.
 *   const util::HostAllocStats s = util::host_alloc_stats();
 *   const long long vivas = (long long)(s.small_allocs + s.large_allocs) -
 *                           (long long)(s.small_frees + s.remote_frees +
 *                                       s.large_frees);
 *   std::printf("%lld vivas, %.1f MiB comprometidos\n", vivas,
 *               s.bytes_reserved / (1024.0 * 1024.0));
 *
 *   // Y cuanto de eso sigue sin declarar su proposito, que es lo que queda
 *   // por migrar.
 *   const uint64_t sin_declarar = s.by_tag[util::AllocTag{}.raw()];
 * @endcode
 *
 * \~
 */
HostAllocStats host_alloc_stats();

/**
 * @brief
 * \~english How much ADDRESS SPACE the region has reserved, in bytes.
 * \~spanish Cuanto ESPACIO DE DIRECCIONES tiene reservado la region, en bytes.
 * \~
 *
 * \~english
 * It is not memory: reserving only sets addresses aside and costs nothing until
 * they are COMMITTED.  What is committed is a different figure and was already
 * visible (@c HostAllocStats::bytes_reserved); this one was missing, and
 * without both there is no answering "how much does this take up", which is the
 * question everyone who picks up the library asks.
 *
 * Zero if the region has not been set up yet -- it is set up on the first large
 * allocation -- which is also an answer.
 *
 * \~spanish
 * No es memoria: reservar solo aparta direcciones y no cuesta nada hasta que se
 * COMPROMETE.  Lo comprometido es otra cifra y ya estaba a la vista
 * (@c HostAllocStats::bytes_reserved); esta faltaba, y sin las dos no se puede
 * responder a "cuanto ocupa esto", que es la pregunta que se hace todo el que
 * se lleva la libreria.
 *
 * Cero si la region aun no se ha montado -- se monta en la primera reserva
 * grande --, lo que tambien es una respuesta.
 *
 * \~
 * @return
 * \~english bytes of address space set aside, or 0 if there is no region yet.
 * \~spanish bytes de espacio de direcciones apartados, o 0 si aun no hay region.
 * \~
 *
 * @par Threads
 * \~english Safe from any thread.  It is one relaxed atomic load.
 * \~spanish Segura desde cualquier hilo.  Es una lectura atomica relajada.
 * \~
 *
 * @code
 *   std::printf("%.1f MiB / %.1f MiB\n",
 *               util::host_region_reserved() / (1024.0 * 1024.0),
 *               util::host_alloc_stats().bytes_reserved / (1024.0 * 1024.0));
 * @endcode
 */
size_t host_region_reserved() noexcept;

/**
 * @brief
 * \~english How many times `operator new` has been entered.  Zero when not
 *           measuring.
 * \~spanish Cuantas veces ha entrado `operator new`.  Cero si no se esta
 *           midiendo.
 * \~
 *
 * @return
 * \~english entries through the C++ door, or 0 if measurement is off.
 * \~spanish entradas por la puerta de C++, o 0 si la medida esta apagada.
 * \~
 *
 * \~english
 * WHY `HostAllocStats` IS NOT ENOUGH.  That one counts ALLOCATIONS served;
 * this one counts ENTRIES through the language's door.  They should match, and
 * when they do not is exactly when you need to know: with only one of the two,
 * a mismatch between the site table and the purpose split cannot be pinned on
 * either side, and you end up deducing it by elimination.
 *
 * It is kept per thread and without atomics -- one cache line per owner -- so
 * it adds no contention where there was none.  The only approximate figure is
 * the one for threads that got no cache of their own, which can overwrite each
 * other; that is normally zero.
 *
 * \~spanish
 * POR QUE NO BASTA CON `HostAllocStats`.  Esa cuenta RESERVAS servidas; esta
 * cuenta ENTRADAS por la puerta del lenguaje.  Deberian coincidir, y cuando no
 * coinciden es cuando hace falta saberlo: con una sola de las dos, un descuadre
 * entre la tabla de sitios y el reparto por proposito no se puede atribuir a
 * ninguna de las dos partes, y se acaba deduciendo por eliminacion.
 *
 * Se lleva por hilo y sin atomicos -- una linea de cache por dueno --, asi que
 * no introduce contencion donde no la habia.  La unica cifra aproximada es la
 * de los hilos que se quedaron sin cache propio, que pueden pisarse entre
 * ellos; normalmente es cero.
 *
 * \~
 * @par Threads
 * \~english Safe.  It adds up plain loads, so it may see a half-updated count
 * from a thread that is allocating at that instant.
 * \~spanish Segura.  Suma lecturas simples; puede ver una cuenta a medio
 * actualizar de un hilo que este reservando en ese instante.
 * \~
 */
uint64_t host_new_calls() noexcept;

/**
 * @brief
 * \~english A SINGLE-OWNER allocator, with nothing to synchronise.
 * \~spanish Un asignador de UN SOLO DUEÑO, sin nada que sincronizar.
 * \~
 *
 * \~english
 * It LIVES ALONGSIDE the general one, it does not replace it, and that is why
 * it is NOT a build option: that would force choosing one of the two for the
 * whole program, and what is needed is being able to mix them.  The blocks are
 * interchangeable: one that comes out of here can be released with
 * @c host_free -- or reach `operator delete`, which is what will happen if a
 * `std::vector` releases it -- and will end up where it should.
 *
 * WHAT IS SAVED.  On the general one's fast path there is not one atomic: the
 * lists are already per thread.  What costs is not synchronising, it is
 * REACHING the cache, reading the thread slot on every allocation.  This one
 * holds the pointer, so it skips that read and its check; and on freeing it
 * also saves comparing the owner.
 *
 * WHAT IT IS FOR.  For whatever has a declared owner and is not shared: a
 * compilation phase, a worker thread with a store of its own, a third-party
 * library we know is used from one thread.  **Never** behind `operator new`,
 * which serves the whole process.
 *
 * @par Threads
 * **It is NOT safe, and THAT is the point.**  One of these objects is used by
 * ONE thread.  What IS safe is ANOTHER thread releasing one of its blocks: that
 * goes through the usual atomic stack, exactly as between ordinary threads.
 *
 * LARGE allocations still go through the common path, which does take a lock:
 * they are one in a hundred thousand operations and duplicating the span
 * machinery would not pay.  What is saved here is the hot path.
 *
 * \~spanish
 * CONVIVE con el general, no lo sustituye, y por eso NO es una opcion de
 * compilacion: eso obligaria a elegir una de las dos para todo el programa, y
 * lo que hace falta es poder mezclarlas.  Los bloques son intercambiables: uno
 * que salga de aqui se puede soltar con @c host_free -- o llegar a
 * `operator delete`, que es lo que pasara si lo suelta un `std::vector` -- y
 * acabara donde debe.
 *
 * QUE SE AHORRA.  En el camino rapido del general no hay ni un atomico: las
 * listas ya son por hilo.  Lo que cuesta no es sincronizar, es LLEGAR al cache,
 * leyendo la ranura del hilo en cada reserva.  Este tiene el puntero, asi que
 * se salta esa lectura y su comprobacion; y al soltar se ahorra ademas comparar
 * el dueño.
 *
 * PARA QUE SIRVE.  Para lo que tiene dueño declarado y no se comparte: una fase
 * de compilacion, un hilo trabajador con su propio almacen, una libreria de
 * terceros que sabemos que se usa desde un hilo.  **Nunca** detras de
 * `operator new`, que sirve a todo el proceso.
 *
 * @par Hilos
 * **NO es segura, y ESE es el punto.**  Un objeto de estos lo usa UN hilo.  Lo
 * que si es seguro es que OTRO hilo suelte un bloque suyo: eso va por la pila
 * atomica de siempre, igual que entre hilos normales.
 *
 * Las reservas GRANDES siguen pasando por el camino comun, que si toma un
 * cerrojo: son una de cada cien mil operaciones y no compensa duplicar la
 * maquinaria de tramos.  Lo que se ahorra aqui es el camino caliente.
 *
 * \~
 *
 * \~english
 * @code
 *   util::SingleOwnerAllocator local;       // its own, for this thread
 *   void *p = local.alloc(64);
 *   local.free(p);
 *   util::host_free(local.alloc(64));       // and this works too
 * @endcode
 *
 * \~spanish
 * @code
 *   util::SingleOwnerAllocator local;       // suyo, de este hilo
 *   void *p = local.alloc(64);
 *   local.free(p);
 *   util::host_free(local.alloc(64));       // y esto tambien vale
 * @endcode
 *
 * \~
 */
class SingleOwnerAllocator {
  public:
    /**
     * @brief
     * \~english Registers a store of its own.
     * \~spanish Da de alta un almacen propio.
     * \~
     *
     * \~english
     * If none are left, @c valid() comes out false and everything is served
     * through the general path -- which works the same, only going through the
     * thread slot.  Degrading like that is on purpose: running out of stores
     * cannot turn into a failure of the program using it.
     *
     * \~spanish
     * Si no quedan, @c valid() sale false y todo se sirve por el camino
     * general -- que funciona igual, solo que pasando por la ranura del hilo.
     * Degradar asi es a proposito: quedarse sin almacen no puede convertirse en
     * un fallo del programa que lo usa.
     *
     * \~
     */
    SingleOwnerAllocator() noexcept;

    /**
     * @brief
     * \~english Gives the id back to the pool.
     * \~spanish Devuelve el identificador al reparto.
     * \~
     *
     * \~english
     * Whatever is left inside is NOT freed: they are valid blocks of chunks
     * that are still ours, and whoever takes the id afterwards inherits them.
     * Without this, every store created and destroyed took an id away forever
     * -- the same flaw the threads used to have -- and once the 63 run out the
     * whole process is served from the shared lists, behind the one lock.
     *
     * \~spanish
     * Lo que quedara dentro NO se libera: son bloques validos de trozos que
     * siguen siendo nuestros, y quien tome el identificador despues los hereda.
     * Sin esto, cada almacen propio que se creara y se destruyera se llevaba un
     * identificador para siempre -- el mismo defecto que tenian los hilos --, y
     * al agotarse los 63 todo el proceso pasa a servirse por las listas
     * compartidas, detras del unico cerrojo.
     *
     * \~
     */
    ~SingleOwnerAllocator() noexcept;

    SingleOwnerAllocator(const SingleOwnerAllocator &) = delete;
    SingleOwnerAllocator &operator=(const SingleOwnerAllocator &) = delete;

    /**
     * @brief
     * \~english Whether it got a store of its own.
     * \~spanish Si consiguio almacen propio.
     * \~
     * @return
     * \~english true when it did; false means everything goes through the
     *           general path, which still works.
     * \~spanish true si lo consiguio; false significa que todo va por el camino
     *           general, que sigue funcionando.
     * \~
     */
    bool valid() const noexcept { return cache_ != nullptr; }

    /**
     * @brief
     * \~english Serves @p n bytes.  Without reading any thread slot.
     * \~spanish Sirve @p n bytes.  Sin leer ninguna ranura de hilo.
     * \~
     * @param n
     * \~english how many bytes are wanted.
     * \~spanish cuantos bytes se quieren.
     * \~
     * @return
     * \~english the block, or nullptr if there was no memory.
     * \~spanish el bloque, o nullptr si no habia memoria.
     * \~
     */
    [[gnu::always_inline]] void *alloc(size_t n) noexcept {
        if (cache_ == nullptr || n - 1 >= kMaxSmall) return host_alloc(n);
        if (detail::g_measure) detail::record_size(cache_, n);
        const uint32_t k = class_of(n);
        void *p = detail::pop_block(cache_, k);
        if (p == nullptr) return detail::host_alloc_refill(cache_, k, n);
        return p;
    }

    /**
     * @brief
     * \~english Returns a block.  Works even if somebody else allocated it.
     * \~spanish Devuelve un bloque.  Vale aunque lo reservara otro.
     * \~
     *
     * \~english
     * IT LOOKS AT BOTH REGIONS, and that is not a detail.  Size classes from
     * `kBigClassMin` upwards live in the BIG region, so `in_region` says no and
     * this used to fall through to the general path -- which re-derives the
     * owner from the THREAD SLOT, not from this allocator.  Because a
     * single-owner id is decoupled from any thread, the two never matched:
     * every 4 KiB free ended up on the remote stack, paying a
     * `compare_exchange`.
     *
     * Measured before touching anything, 20,000 blocks per size: at 64 and
     * 1024 bytes all 20,000 frees were local; at 4096 and 8192, all 20,000
     * were REMOTE.  That is what made the single-owner column 2-3x worse than
     * the shared one from exactly that size upwards.
     *
     * The small path pays nothing: the second question is only asked once the
     * first has already said no, which is precisely where this used to give up.
     *
     * \~spanish
     * MIRA LAS DOS REGIONES, y no es un detalle.  Las clases de tamano de
     * `kBigClassMin` en adelante viven en la region GRANDE, asi que `in_region`
     * dice que no y esto se caia al camino general -- que vuelve a deducir el
     * dueño de la RANURA DEL HILO, no de este asignador --.  Como el
     * identificador de un solo dueño esta desligado de cualquier hilo, los dos
     * no coincidian nunca: toda liberacion de 4 KiB acababa en la pila remota,
     * pagando un `compare_exchange`.
     *
     * Medido antes de tocar nada, 20.000 bloques por tamano: a 64 y a 1024
     * bytes las 20.000 liberaciones fueron locales; a 4096 y a 8192, las 20.000
     * fueron REMOTAS.  Eso es lo que hacia que la columna de un solo dueño
     * fuera 2-3x peor que la compartida justo a partir de ese tamano.
     *
     * El camino pequeno no paga nada: la segunda pregunta solo se hace cuando
     * la primera ya ha dicho que no, que es exactamente donde esto se rendia.
     *
     * \~
     * @param p
     * \~english the block to give back, or nullptr.
     * \~spanish el bloque a devolver, o nullptr.
     * \~
     *
     * \~english
     * @code
     *   util::SingleOwnerAllocator a;
     *   void *small = a.alloc(64);    // small region
     *   void *big   = a.alloc(4096);  // big region
     *   a.free(big);                  // local push, no atomic
     *   a.free(small);
     * @endcode
     *
     * \~spanish
     * @code
     *   util::SingleOwnerAllocator a;
     *   void *pequeno = a.alloc(64);    // region pequena
     *   void *grande  = a.alloc(4096);  // region grande
     *   a.free(grande);                 // empuje local, sin atomicos
     *   a.free(pequeno);
     * @endcode
     *
     * \~
     */
    /* RAMIFICA UNA VEZ Y DELEGA CON LA CABECERA EN LA MANO, por el mismo motivo
     * que @c PerThreadAllocator::free, donde esta contado: ceder a `host_free`
     * lo que no es nuestro le hacia repetir la pregunta de la region y la
     * lectura de la cabecera que aqui ya se habian hecho.  Perfilado, esta
     * funcion era la mas cara de las tres vias de liberar -- 0,141 s contra
     * 0,074 de `host_free` --, que es justo al reves de lo que una via
     * especializada deberia salir. */
    [[gnu::always_inline]] void free(void *p) noexcept {
        if (p == nullptr) return;

        /* Una rama por region, cada una acabando en el camino exacto con la
         * cabecera ya en la mano; las otras dos formas que se probaron salieron
         * peores, y estan contadas en @c PerThreadAllocator::free. */
        if (in_region(p)) {
            ChunkHeader *h = chunk_of(p);
            if (__builtin_expect(h->magic != kChunkMagic, 0)) {
                detail::host_free_not_small(p, h); // un tramo, o algo roto
                return;
            }
            if (cache_ != nullptr && h->owner == cache_->id) {
                detail::push_block(cache_, p, h->cls);
                return;
            }
            detail::host_free_remote(p, h);
            return;
        }

        if (in_big_region(p)) {
            ChunkHeader *h = big_chunk_of(p);
            if (cache_ != nullptr && h->magic == kChunkMagic &&
                h->owner == cache_->id) {
                detail::push_block(cache_, p, h->cls);
                return;
            }
            detail::host_free_big(p);
            return;
        }

        // Del sistema en reserva propia, o de nadie; ver `host_free`.
        detail::host_free_outside(p);
    }

    /**
     * @brief
     * \~english What THIS store has counted, adding up nobody else's.
     * \~spanish Lo que lleva contado ESTE almacen, sin sumar el de nadie mas.
     * \~
     *
     * \~english
     * By VALUE and not by reference on purpose: `small_allocs` is not
     * incremented on the hot path -- the total is the sum of `by_tag`, which is
     * what makes counting by purpose free -- so it has to be filled in when
     * asked for.  Returning a reference, that field would ALWAYS come out zero
     * and nobody would notice: data that lies quietly is worse than data that
     * is missing.
     *
     * \~spanish
     * Por VALOR y no por referencia a proposito: `small_allocs` no se
     * incrementa en el camino caliente -- el total es la suma de `by_tag`, que
     * es lo que hace que contar por proposito salga gratis --, asi que hay que
     * rellenarlo al pedirlo.  Devolviendo una referencia, ese campo saldria
     * SIEMPRE a cero y nadie se enteraria: un dato que miente en silencio es
     * peor que uno que falta.
     *
     * \~
     * @return
     * \~english a copy of this store's counters, already consistent.
     * \~spanish una copia de los contadores de este almacen, ya coherentes.
     * \~
     */
    HostAllocStats stats() const noexcept;

  private:
    detail::ThreadCache *cache_;
    /**
     * \~english
     * What the cache had already counted when this store took it, so that
     * @c stats reports THIS store's work and not its predecessor's.
     *
     * WHY IT APPEARED.  Owner ids are recycled now, so a new store is handed a
     * cache a previous owner used, with its counters still in it -- and a
     * per-store counter that opens showing someone else's allocations is not a
     * per-store counter.  Before the ids came back, every store got a virgin
     * one and the question never arose.
     *
     * AND WHY A BASELINE INSTEAD OF ZEROING THE CACHE, which would be shorter:
     * `host_alloc_stats` builds the global figures by SUMMING these very
     * counters, so clearing them would quietly delete the previous owner's
     * work from the process total.  Subtracting keeps both answers true.
     *
     * \~spanish
     * Lo que el cache ya llevaba contado cuando este almacen lo cogio, para que
     * @c stats cuente lo de ESTE almacen y no lo de su antecesor.
     *
     * DE DONDE SALE.  Los identificadores de dueno ya se reciclan, asi que a un
     * almacen nuevo le toca un cache que uso otro, con sus contadores dentro --
     * y un contador por almacen que abre ensenando las reservas de otro no es
     * un contador por almacen.  Antes de que los identificadores volvieran,
     * cada almacen cogia uno virgen y la pregunta no se planteaba.
     *
     * Y POR QUE UNA LINEA DE PARTIDA Y NO PONER EL CACHE A CERO, que seria mas
     * corto: `host_alloc_stats` forma las cifras globales SUMANDO justo estos
     * contadores, asi que limpiarlos borraria en silencio del total del proceso
     * lo que hizo el dueno anterior.  Restando, las dos respuestas siguen
     * siendo ciertas.
     * \~
     */
    HostAllocStats base_;
};

/**
 * @brief
 * \~english Many threads, and NOT ONE LOCK on the small path.
 * \~spanish Muchos hilos, y NI UN CERROJO en el camino pequeno.
 * \~
 *
 * \~english
 * WHAT IT IS FOR.  The process-wide allocator bounds MEMORY: it can name at
 * most `kMaxThreads` owners, and a thread that does not get one is served from
 * the shared lists behind a spin lock.  That bound is the right default for
 * something that serves the whole process, but it has a price, and the price
 * was measured: past the point where the threads on the shared path outnumber
 * the cores, the lock stops being a wait and becomes a convoy.  With 20,000
 * allocations per thread on a 24-core machine, CPU time per operation went
 * 46.5 -> 167.4 -> 537.1 ns at 21, 25 and 65 threads on the shared path, doing
 * exactly the same work.
 *
 * This type bounds LATENCY instead.  Every thread gets a cache of its own, so
 * allocating and freeing take no lock ever, and there is no shared fallback to
 * fall into.  What it costs is memory: one cache per live thread, out of a pool
 * of `kPerThreadCaches`.
 *
 * NEITHER IS BETTER.  They bound different things, and that is exactly why both
 * exist rather than one replacing the other.  Pick this one when the thread
 * count is known and latency matters; keep the shared one when threads are
 * unbounded and memory must not grow with them.
 *
 * WHAT IS STILL SHARED.  Only allocations up to `kMaxSmall` are lock-free.
 * Anything larger is a span, and spans go through the general path, which does
 * take the span lock -- rarely, since spans are the uncommon case.
 *
 * @par Threads
 * **Safe from any thread.**  Every thread that calls this gets its own cache on
 * its first allocation and gives it back when it dies.  A block may be freed
 * from a different thread than the one that allocated it: that goes down the
 * general path, exactly like everywhere else in this allocator.
 *
 * \~spanish
 * PARA QUE SIRVE.  El asignador de todo el proceso acota la MEMORIA: puede
 * nombrar como mucho `kMaxThreads` dueños, y un hilo que no consiga uno se
 * sirve de las listas compartidas detras de un cerrojo de giro.  Esa cota es lo
 * correcto por defecto para algo que sirve al proceso entero, pero tiene un
 * precio, y el precio esta medido: pasado el punto en que los hilos del camino
 * compartido superan a los nucleos, el cerrojo deja de ser una espera y se
 * convierte en un convoy.  Con 20.000 reservas por hilo en una maquina de 24
 * nucleos, el tiempo de CPU por operacion paso de 46,5 a 167,4 y a 537,1 ns con
 * 21, 25 y 65 hilos en el camino compartido, haciendo exactamente el mismo
 * trabajo.
 *
 * Este tipo acota en cambio la LATENCIA.  Cada hilo recibe un cache propio, asi
 * que reservar y soltar no toman ningun cerrojo nunca, y no hay respaldo
 * compartido en el que caer.  Lo que cuesta es memoria: un cache por hilo vivo,
 * de un reparto de `kPerThreadCaches`.
 *
 * NINGUNO ES MEJOR.  Acotan cosas distintas, y por eso existen los dos en vez
 * de sustituir uno al otro.  Este cuando se sabe cuantos hilos hay y la
 * latencia importa; el compartido cuando los hilos no estan acotados y la
 * memoria no puede crecer con ellos.
 *
 * QUE SIGUE SIENDO COMPARTIDO.  Solo las reservas de hasta `kMaxSmall` van sin
 * cerrojo.  Lo mayor es un tramo, y los tramos pasan por el camino general, que
 * si toma el cerrojo de tramos -- pocas veces, porque los tramos son el caso
 * raro.
 *
 * @par Hilos
 * **Segura desde cualquier hilo.**  Cada hilo que la llame recibe su cache en
 * su primera reserva y lo devuelve al morir.  Un bloque se puede soltar desde
 * un hilo distinto del que lo reservo: eso baja por el camino general,
 * exactamente igual que en todo el resto de este asignador.
 *
 * \~
 *
 * \~english
 * @code
 *   // One instance per call site, or none at all: it holds no state.
 *   util::PerThreadAllocator a;
 *   void *p = a.alloc(256);
 *   a.free(p);
 *
 *   // Equivalent, and this is the form that shows it is a choice of TYPE:
 *   void *q = util::PerThreadAllocator::alloc(256);
 *   util::PerThreadAllocator::free(q);
 * @endcode
 *
 * \~spanish
 * @code
 *   // Una instancia por sitio de llamada, o ninguna: no guarda estado.
 *   util::PerThreadAllocator a;
 *   void *p = a.alloc(256);
 *   a.free(p);
 *
 *   // Equivalente, y esta es la forma que ensena que es una eleccion de TIPO:
 *   void *q = util::PerThreadAllocator::alloc(256);
 *   util::PerThreadAllocator::free(q);
 * @endcode
 *
 * \~
 */
class PerThreadAllocator {
  public:
    /**
     * @brief
     * \~english Serves @p n bytes without taking any lock.
     * \~spanish Sirve @p n bytes sin tomar ningun cerrojo.
     * \~
     *
     * \~english
     * Falls back to the general path in two cases, and both are the uncommon
     * one: a span (larger than `kMaxSmall`), and a pool with no id left.
     *
     * \~spanish
     * Se cae al camino general en dos casos, y los dos son el raro: un tramo
     * (mayor que `kMaxSmall`), y un reparto sin identificadores libres.
     *
     * \~
     * @param n
     * \~english how many bytes are wanted.
     * \~spanish cuantos bytes se quieren.
     * \~
     * @return
     * \~english the block, or nullptr if there was no memory.
     * \~spanish el bloque, o nullptr si no habia memoria.
     * \~
     */
    [[gnu::always_inline]] static void *alloc(size_t n) noexcept {
        /* THE SIZE FIRST, and the order is the point.  A span goes down the
         * general path whatever this thread's cache says, so reading the slot
         * before knowing the size was reading it to throw it away -- and then
         * `host_alloc` read its own.  Same single jump as ever: with `n == 0`
         * the subtraction wraps to the largest unsigned, which also falls
         * outside, so the size is validated without a second comparison. */
        if (__builtin_expect(n - 1 >= kMaxSmall, 0))
            return host_alloc(n); // spans: general path
        detail::ThreadCache *c = detail::per_thread_cache();
        /* One unsigned comparison for "not registered yet" and "already had its
         * exit notice"; see @c detail::kDyingCache. */
        if (__builtin_expect(!detail::have_cache(c), 0)) {
            /* Past the notice there is no cache to be had, and asking would
             * take an id this thread can no longer give back. */
            if (c != nullptr) return host_alloc(n);
            c = detail::per_thread_cache_slow();
            if (c == nullptr) return host_alloc(n); // pool exhausted
        }
        if (detail::g_measure) detail::record_size(c, n);
        const uint32_t k = class_of(n);
        void *p = detail::pop_block(c, k);
        if (p == nullptr) return detail::host_alloc_refill(c, k, n);
        return p;
    }

    /**
     * @brief
     * \~english Returns a block.  Works even if another thread allocated it.
     * \~spanish Devuelve un bloque.  Vale aunque lo reservara otro hilo.
     * \~
     *
     * \~english
     * Looks at BOTH regions before giving up, for the same reason
     * @c SingleOwnerAllocator::free does: size classes from `kBigClassMin`
     * upwards live in the big region, so asking only `in_region` would send
     * every one of those frees down the general path -- and there the owner is
     * re-derived from the OTHER thread slot, which never matches, turning each
     * one into an atomic push.
     *
     * \~spanish
     * Mira LAS DOS regiones antes de rendirse, por lo mismo que
     * @c SingleOwnerAllocator::free: las clases de tamano de `kBigClassMin` en
     * adelante viven en la region grande, asi que preguntar solo `in_region`
     * mandaria todas esas liberaciones por el camino general -- y ahi el dueño
     * se vuelve a deducir de la OTRA ranura de hilo, que no coincide nunca,
     * convirtiendo cada una en un empuje atomico.
     *
     * \~
     * @param p
     * \~english the block to give back, or nullptr.
     * \~spanish el bloque a devolver, o nullptr.
     * \~
     */
    /**
     * @brief Frees a block, from this thread or any other.
     *
     * IT USED TO HAND WHAT IT DID NOT OWN TO `host_free`, AND THAT WAS WORK
     * DONE TWICE.  This function already asks which region the pointer is in
     * and already reads the chunk header; `host_free` then asked both again
     * from scratch.  Every span went through that -- a span's header carries
     * `kSpanMagic`, so the test here never matches -- and it showed: profiled,
     * the header read cost 0.071 s here and another 0.066 s inside `host_free`,
     * about a tenth of all the CPU this library spent.  It is also why the two
     * specialised policies came out SLOWER than the general one at 64 KiB and
     * 1 MiB, which is backwards from the point of specialising.
     *
     * Now it branches once and calls the exact path with the header already in
     * hand.  Nothing is re-derived, and each of the four outcomes -- ours, a
     * span, another thread's, not ours at all -- goes where it belongs.
     */
    [[gnu::always_inline]] static void free(void *p) noexcept {
        if (p == nullptr) return;
        detail::ThreadCache *c = detail::per_thread_cache();

        /* ONE BRANCH PER REGION, each ending in the exact path with the header
         * already in hand.  Two other shapes were written and measured against
         * this one, both worse -- normalising to the general column, which this
         * change does not touch, so it doubles as a control inside each run:
         *
         *   per-thread / general      64 B    64 KiB    1 MiB
         *   before                    1.029    1.257    1.195
         *   this                      1.007    1.063    1.008
         *   header resolved once      1.035    1.132    1.124
         *
         * The last one reads better on paper -- one copy of the test instead of
         * one per region -- and is slower, which is why it is not here. */
        if (in_region(p)) {
            ChunkHeader *h = chunk_of(p);
            if (__builtin_expect(h->magic != kChunkMagic, 0)) {
                // A span, or something that should not be here.  Both cold.
                detail::host_free_not_small(p, h);
                return;
            }
            if (detail::have_cache(c) && h->owner == c->id) {
                detail::push_block(c, p, h->cls);
                return;
            }
            detail::host_free_remote(p, h);
            return;
        }

        if (in_big_region(p)) {
            ChunkHeader *h = big_chunk_of(p);
            if (detail::have_cache(c) && h->magic == kChunkMagic &&
                h->owner == c->id) {
                detail::push_block(c, p, h->cls);
                return;
            }
            detail::host_free_big(p);
            return;
        }

        /* OUTSIDE BOTH REGIONS: either a block the system served on a
         * reservation of its own, or nothing of ours -- and that second one
         * cannot happen, because if nothing falls back to the system when
         * allocating there are no foreign blocks to release.  Same reasoning
         * as `host_free`, and the same place it is decided. */
        detail::host_free_outside(p);
    }
};

/**
 * @brief
 * \~english How many times @c PerThreadAllocator ran out of caches.
 * \~spanish Cuantas veces @c PerThreadAllocator se quedo sin caches.
 * \~
 *
 * \~english
 * IT HAS TO BE ASKABLE.  This policy has no shared fallback on purpose, so
 * exhausting the pool means threads silently going down the general path --
 * that is, back behind the lock this type exists to avoid.  Non-zero here means
 * `kPerThreadCaches` is too small for the program, and the only symptom
 * otherwise would be "it got slower for no visible reason".
 *
 * \~spanish
 * TIENE QUE PODER PREGUNTARSE.  Esta politica no tiene respaldo compartido a
 * proposito, asi que agotar el reparto significa hilos bajando en silencio por
 * el camino general -- o sea, otra vez detras del cerrojo que este tipo existe
 * para evitar --.  Distinto de cero aqui significa que `kPerThreadCaches` se le
 * queda pequeno al programa, y el unico sintoma si no seria "se puso mas lento
 * sin razon visible".
 *
 * \~
 * @return
 * \~english times a thread asked for a per-thread cache and none was left.
 * \~spanish veces que un hilo pidio cache propio y no quedaba ninguno.
 * \~
 *
 * \~english
 * @code
 *   if (util::host_per_thread_exhausted() != 0)
 *       report("more live threads than per-thread caches");
 * @endcode
 *
 * \~spanish
 * @code
 *   if (util::host_per_thread_exhausted() != 0)
 *       informar("mas hilos vivos que caches por hilo");
 * @endcode
 *
 * \~
 */
uint64_t host_per_thread_exhausted() noexcept;

/**
 * @brief
 * \~english How many allocations the system served on a reservation of their
 *          own.
 * \~spanish Cuantas reservas sirvio el sistema en una reserva propia.
 * \~
 *
 * \~english
 * Everything above @c kMaxSpanBytes goes that way, because past that size the
 * region cannot recycle and its commit/decommit pair costs far more than a
 * private reservation -- see the constant, which carries the measurement.
 *
 * \~spanish
 * Todo lo que pasa de @c kMaxSpanBytes va por ahi, porque a partir de ese
 * tamano la region no puede reciclar y su par comprometer/descomprometer sale
 * mucho mas caro que una reserva propia -- ver la constante, que lleva la
 * medida.
 *
 * \~
 * @return
 * \~english how many were served that way since the process started.
 * \~spanish cuantas se sirvieron asi desde que arranco el proceso.
 * \~
 */
uint64_t host_direct_allocs() noexcept;

/**
 * @brief
 * \~english How many big zeroed blocks were served under each declaration of
 *          how much would be touched.
 * \~spanish Cuantos bloques grandes a cero se sirvieron bajo cada declaracion
 *          de cuanto se iba a tocar.
 * \~
 *
 * \~english
 * IT HAS TO BE ASKABLE, because what @c AllocFill carries is an ASSERTION the
 * allocator then acts on -- it changes which of two opposite mechanisms serves
 * the block.  An assertion nobody can look at is folklore: this says how many
 * took each branch, and what came in as @c AllocFill::Unknown is exactly what
 * has not been declared yet.
 *
 * It is a global count and not part of @c HostAllocStats on purpose: that
 * structure sits inside the per-thread cache ahead of the hot batch, so a
 * counter added to it moves the hot path -- see the note where it is defined.
 *
 * \~spanish
 * TIENE QUE PODER PREGUNTARSE, porque lo que lleva @c AllocFill es una
 * AFIRMACION sobre la que el asignador actua -- cambia cual de dos mecanismos
 * opuestos sirve el bloque --.  Una afirmacion que nadie puede mirar es
 * folclore: esto dice cuantas fueron por cada rama, y lo que entre como
 * @c AllocFill::Unknown es exactamente lo que todavia no declara nada.
 *
 * Es una cuenta global y no parte de @c HostAllocStats a proposito: esa
 * estructura vive dentro del cache por hilo por delante del lote caliente, asi
 * que un contador ahi mueve el camino rapido -- ver la nota donde se define.
 *
 * \~
 * @param f
 * \~english which declaration to ask about.
 * \~spanish por cual declaracion se pregunta.
 * \~
 * @return
 * \~english how many were served under it since the process started.
 * \~spanish cuantos se sirvieron bajo ella desde que arranco el proceso.
 * \~
 *
 * \~english
 * @code
 *   const uint64_t undeclared = util::host_fill_allocs(util::AllocFill::Unknown);
 * @endcode
 *
 * \~spanish
 * @code
 *   const uint64_t sin_declarar =
 *       util::host_fill_allocs(util::AllocFill::Unknown);
 * @endcode
 *
 * \~
 */
uint64_t host_fill_allocs(AllocFill f) noexcept;

/**
 * @brief
 * \~english Times a block big enough had to go back to the region because the
 *          table of direct blocks was full.
 * \~spanish Veces que un bloque bastante grande tuvo que volver a la region
 *          porque la tabla de bloques directos estaba llena.
 * \~
 *
 * \~english
 * IT HAS TO BE ASKABLE, for the same reason as
 * @c host_per_thread_exhausted: falling back works, so the only symptom of a
 * @c kDirectSlots that is too small for the program would be the slower path
 * coming back with nothing said.
 *
 * \~spanish
 * TIENE QUE PODER PREGUNTARSE, por lo mismo que
 * @c host_per_thread_exhausted: el respaldo funciona, asi que el unico sintoma
 * de un @c kDirectSlots que se le queda pequeno al programa seria el camino
 * lento volviendo sin que nada lo diga.
 *
 * \~
 * @return
 * \~english how many went back to the region for want of a slot.
 * \~spanish cuantas volvieron a la region por falta de sitio.
 * \~
 */
uint64_t host_direct_refused() noexcept;

/**
 * @brief
 * \~english Hands every parked span back to the shared pool, where it is
 *          merged.
 * \~spanish Devuelve al reparto comun todos los tramos guardados, donde se
 *          funden.
 * \~
 *
 * \~english
 * WHY THIS EXISTS.  The lock-free span policies buy their speed by parking
 * freed spans somewhere no coalescer can see them -- that is exactly what makes
 * reaching them cost no lock.  The price is that two spans lying next to each
 * other stay apart, so a later request bigger than either of them cannot be
 * served from the pair.  This is the way to pay that back: a phase boundary,
 * where a program knows it has finished with one working set and is about to
 * ask for a different one.
 *
 * It is also what makes coalescing TESTABLE under those policies: without it,
 * "were these two merged?" has no answer, because they never reach the pool.
 *
 * Under the locked policy there is nothing parked and this returns zero.
 *
 * @par Threads
 * **Safe from any thread**, but it takes the span lock repeatedly and merges as
 * it goes, so it is a between-phases call and not something for a hot loop.
 *
 * \~spanish
 * POR QUE EXISTE.  Las politicas de tramos sin cerrojos compran su velocidad
 * guardando los tramos liberados donde ningun fusionador puede verlos -- que es
 * justo lo que hace que llegar a ellos no cueste ningun cerrojo --.  El precio
 * es que dos tramos que estan uno al lado del otro siguen separados, asi que
 * una peticion posterior mayor que cualquiera de los dos no se puede servir con
 * la pareja.  Esta es la forma de devolver eso: una frontera de fase, donde un
 * programa sabe que ha terminado con un conjunto de trabajo y va a pedir otro
 * distinto.
 *
 * Es tambien lo que hace COMPROBABLE la fusion con esas politicas: sin esto,
 * "se fundieron estos dos?" no tiene respuesta, porque no llegan nunca al
 * reparto.
 *
 * Con la politica con cerrojo no hay nada guardado y esto devuelve cero.
 *
 * @par Hilos
 * **Segura desde cualquier hilo**, pero toma el cerrojo de tramos varias veces
 * y va fundiendo sobre la marcha, asi que es una llamada de entre fases y no
 * algo para un bucle caliente.
 *
 * \~
 * @return
 * \~english bytes moved back into the shared pool.
 * \~spanish bytes devueltos al reparto comun.
 * \~
 *
 * \~english
 * @code
 *   run_parsing_phase();
 *   util::host_span_trim();   // let the next phase reuse what parsing freed
 *   run_codegen_phase();
 * @endcode
 *
 * \~spanish
 * @code
 *   fase_de_analisis();
 *   util::host_span_trim();   // que la fase siguiente reuse lo que solto
 *   fase_de_generacion();
 * @endcode
 *
 * \~
 */
size_t host_span_trim() noexcept;

/**
 * @brief
 * \~english Allocates @p n bytes, ZEROED.
 * \~spanish Sirve @p n bytes PUESTOS A CERO.
 * \~
 *
 * @param n
 * \~english bytes wanted.
 * \~spanish bytes que se quieren.
 * \~
 * @return
 * \~english the zeroed block, or nullptr when there is no memory left.
 * \~spanish el bloque a cero, o nullptr cuando ya no hay memoria.
 * \~
 *
 * \~english
 * It is NOT `host_alloc` plus a `memset`, and that is the whole point: memory
 * that has just come from the operating system **arrives zeroed already** --
 * both Windows and POSIX guarantee it, because handing over another process's
 * pages without clearing them would leak data -- so zeroing it again is writing
 * for nothing.
 *
 * How much for nothing, measured on Linux against `calloc` with 1 MiB blocks:
 *
 *     always memset          176,126 ns
 *     glibc calloc             7,578 ns
 *
 * Twenty-three times, and not by being slower at the same work: by doing work
 * that was not needed.  Small blocks ARE cleared, because they come off a free
 * list and carry whatever the previous tenant left.
 *
 * \~spanish
 * NO es `host_alloc` mas un `memset`, y ahi esta toda la gracia: cuando la
 * memoria acaba de venir del sistema operativo **ya viene a cero** -- lo
 * garantizan tanto Windows como POSIX, porque entregar paginas de otro proceso
 * sin limpiarlas seria una fuga de datos --, asi que volver a ponerla a cero es
 * escribir de balde.
 *
 * Cuanto de balde, medido en Linux contra `calloc` con bloques de 1 MiB:
 *
 *     con memset siempre     176.126 ns
 *     calloc de glibc          7.578 ns
 *
 * Veintitres veces, y no por ser mas lento haciendo lo mismo: por hacer un
 * trabajo que no hacia falta.  Los bloques pequenos SI se limpian, porque salen
 * de una lista de libres y llevan lo que dejara el inquilino anterior.
 *
 * \~
 * @par Threads
 * \~english Safe from any thread, the same as @c host_alloc.
 * \~spanish Segura desde cualquier hilo, igual que @c host_alloc.
 * \~
 *
 * @code
 *   int *v = static_cast<int *>(util::host_alloc_zeroed(n * sizeof(int)));
 * @endcode
 */
void *host_alloc_zeroed(size_t n) noexcept;

/**
 * @brief
 * \~english USABLE bytes of @p p, which may be more than were asked for.
 * \~spanish Bytes UTILIZABLES de @p p, que pueden ser mas de los que se pidieron.
 * \~
 *
 * \~english
 * Rounding up to a size class is not waste if it gets used: asking for 40
 * bytes gives 48, and those eight are yours.
 *
 * \~spanish
 * El redondeo a clase no es desperdicio si se aprovecha: pedir 40 bytes da 48,
 * y esos ocho son tuyos.
 *
 * \~
 * @param p
 * \~english a block from this allocator, or nullptr.
 * \~spanish un bloque de este asignador, o nullptr.
 * \~
 * @return
 * \~english how much can be written, or 0 if @p p did not come from here.
 * \~spanish cuanto se puede escribir, o 0 si @p p no salio de aqui.
 * \~
 *
 * @par Threads
 * \~english Safe from any thread.  It only reads the chunk header.
 * \~spanish Segura desde cualquier hilo.  Solo lee la cabecera del trozo.
 * \~
 *
 * @code
 * char *buf = static_cast<char *>(util::host_alloc(40));
 * const size_t room = util::host_usable_size(buf);   // 48
 * @endcode
 */
size_t host_usable_size(const void *p) noexcept;

/**
 * @brief
 * \~english Resizes @p p to @p n bytes.
 * \~spanish Cambia el tamano de @p p a @p n bytes.
 * \~
 *
 * @param p
 * \~english the block to resize, or nullptr to allocate a fresh one.
 * \~spanish el bloque a redimensionar, o nullptr para reservar uno nuevo.
 * \~
 * @param n
 * \~english the new size.  Zero frees the block and returns nullptr.
 * \~spanish el tamano nuevo.  Cero suelta el bloque y devuelve nullptr.
 * \~
 * @return
 * \~english the block, possibly moved; nullptr if it could not be done, and in
 *           that case @p p IS STILL VALID, as `realloc` requires.
 * \~spanish el bloque, quiza movido; nullptr si no se pudo, y en ese caso
 *           @p p SIGUE SIENDO VALIDO, igual que manda `realloc`.
 * \~
 *
 * \~english
 * @code
 * char *buf = static_cast<char *>(util::host_alloc(64));
 * char *bigger = static_cast<char *>(util::host_realloc(buf, 4096));
 * // on failure `buf` is intact and still has to be freed
 * if (bigger == nullptr) { util::host_free(buf); return; }
 * @endcode
 *
 * \~spanish
 * @code
 * char *buf = static_cast<char *>(util::host_alloc(64));
 * char *bigger = static_cast<char *>(util::host_realloc(buf, 4096));
 * // si falla, `buf` sigue entero y hay que soltarlo igual
 * if (bigger == nullptr) { util::host_free(buf); return; }
 * @endcode
 *
 * \~
 *
 * \~spanish
 * Evita copiar en dos casos, y el segundo es el que importa:
 * \~english
 * It avoids copying in two cases, and the second is the one that matters:
 * \~
 *
 * Evita copiar en dos casos, y el segundo es el que importa:
 *
 *  1. Si ya cabe en lo que tiene, devuelve el mismo puntero.
 *  2. Si es una reserva grande y su tramo es **lo ultimo que se tomo de la
 *     region**, lo ESTIRA en su sitio.  Ese es el patron de un bufer que crece
 *     -- reservar, duplicar, duplicar --, y sin esto cada duplicacion copiaba
 *     el contenido entero: crecer hasta 1 MiB costaba 1.450 ns por paso frente
 *     a los 49 de `realloc`, que usa `mremap` y no copia.
 *
 * `mremap` aqui no vale, y conviene saber por que: moveria el bloque FUERA de
 * nuestra region y `in_region` dejaria de reconocerlo.
 *
 * @par Hilos
 * Segura desde cualquier hilo.  Vale aunque @p p lo reservara otro.
 */
void *host_realloc(void *p, size_t n) noexcept;

/**
 * @brief
 * \~english Whether the allocator is actually serving.
 * \~spanish Si el asignador esta sirviendo de verdad.
 * \~
 *
 * \~english
 * The one way to confirm the library really took over instead of assuming it.
 * It matters most with a static archive, where a link that did not pull the
 * objects in leaves the program on the system allocator WITHOUT failing --
 * see the README.
 *
 * \~spanish
 * La forma de confirmar que la libreria de verdad tomo el mando, en vez de
 * suponerlo.  Importa sobre todo con el archivo estatico, donde un enlace que
 * no saco los objetos deja al programa con el asignador del sistema SIN que
 * falle nada -- ver el README.
 *
 * \~
 * @return
 * \~english true when it is serving.  False only while it is still deciding --
 * a window that lasts one call, inside static initialisation -- or when the
 * region could not be reserved at all.
 * \~spanish true cuando esta sirviendo.  False solo mientras aun esta
 * decidiendo -- una ventana de una sola llamada, dentro de la inicializacion de
 * estaticos -- o cuando no se pudo reservar la region.
 * \~
 *
 * @par Threads
 * \~english Safe.  The first call asks the environment; the rest read an atomic.
 * \~spanish Segura.  La primera llamada consulta el entorno; las demas leen un
 * atomico.
 * \~
 *
 * @code
 * if (!util::host_alloc_active())
 *     std::fputs("the allocator is NOT in force\n", stderr);
 * @endcode
 */
bool host_alloc_active();

} // namespace util

#endif // VESTA_UTIL_HOST_ALLOCATOR_H

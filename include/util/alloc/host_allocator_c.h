/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/alloc/host_allocator_c.h
 * @brief
 * \~english The project's allocator, with the signature C understands.
 * \~spanish El asignador del proyecto, con la firma que entiende C.
 * \~
 *
 * \~english
 * WHY IT EXISTS.  Because there is memory we do NOT see today: the libraries
 * written in C that we carry inside -- Capstone, SQLite, OpenSSL, miniz -- call
 * `malloc` directly, so they never go through our `operator new` and appear in
 * no count at all.  Almost all of them offer a hook to swap their allocator,
 * and that hook asks for function pointers with `malloc`'s signature: that is
 * what is here.
 *
 * And it also serves the native plugins, which already speak C
 * (`include/ffi/vesta_plugin.h`).
 *
 * WHAT IS GAINED.  That this memory lands in the same place as ours: the same
 * counters, the same split by purpose, the same region.  With that it becomes
 * possible to finally answer how much each library weighs -- the cache
 * compression, for instance, which we already know takes 5.4% of the
 * instructions of a cold build, but about whose memory we know nothing.
 *
 * WHAT IT COSTS.  From C the fast path cannot be put inside the caller, so one
 * call per allocation is paid.  That is exactly what anyone calling `malloc`
 * already pays, so nothing is lost: it simply does not gain the part that C++
 * does gain.
 *
 * THIS HEADER IS PURE C.  No templates, no references, no namespaces, and it
 * includes no C++ header.  It can be included from a `.c`.
 *
 * \~spanish
 * POR QUE EXISTE.  Porque hay memoria que hoy NO VEMOS: las librerias escritas
 * en C que llevamos dentro -- Capstone, SQLite, OpenSSL, miniz -- llaman a
 * `malloc` directamente, asi que no pasan por nuestro `operator new` y no
 * aparecen en ninguna cuenta.  Casi todas ofrecen un gancho para cambiarles el
 * asignador, y ese gancho pide punteros a funcion con la firma de `malloc`:
 * eso es lo que hay aqui.
 *
 * Y de paso sirve a los plugins nativos, que ya hablan C
 * (`include/ffi/vesta_plugin.h`).
 *
 * QUE SE GANA.  Que esa memoria entre en el mismo sitio que la nuestra: los
 * mismos contadores, el mismo reparto por proposito, la misma region.  Con eso
 * se puede contestar por fin cuanto pesa cada libreria -- por ejemplo la
 * compresion de la cache, que ya sabemos que se lleva un 5,4% de las
 * instrucciones de una compilacion en frio, pero de cuya memoria no sabemos
 * nada.
 *
 * QUE CUESTA.  Desde C no se puede meter el camino rapido dentro de quien
 * llama, asi que se paga una llamada por reserva.  Es exactamente lo que ya
 * paga quien llama a `malloc`, asi que no pierde nada: simplemente no gana la
 * parte que si gana el C++.
 *
 * ESTA CABECERA ES C PURO.  Nada de plantillas, referencias ni namespaces, y no
 * incluye ninguna cabecera de C++.  Se puede incluir desde un `.c`.
 *
 * \~
 */
#ifndef VESTA_UTIL_HOST_ALLOCATOR_C_H
#define VESTA_UTIL_HOST_ALLOCATOR_C_H

#include "util/report/alloc_sites_c.h" /* VESTA_ALLOC_TAG_SLOTS y _SIZE_BUCKETS */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief
 * \~english Serves @p n bytes.  The equivalent of `malloc`.
 * \~spanish Sirve @p n bytes.  Equivalente a `malloc`.
 * \~
 *
 * \~english
 * What comes back is good for any type: it is aligned to 16 bytes.
 *
 * @par Threads
 * **Safe from any thread**, and with no locks on the normal path: every thread
 * has its own lists.  See `util/alloc/host_allocator.h` for the detail.
 *
 * \~spanish
 * Lo devuelto vale para cualquier tipo: sale alineado a 16 bytes.
 *
 * @par Hilos
 * **Segura desde cualquier hilo**, y sin cerrojos en el camino normal: cada
 * hilo tiene sus propias listas.  Ver `util/alloc/host_allocator.h` para el detalle.
 *
 * \~
 * @param n
 * \~english how many bytes are wanted.
 * \~spanish cuantos bytes se quieren.
 * \~
 * @return
 * \~english NULL if it could not be done.  With @p n at zero it returns a valid
 *           block of minimum size, as `malloc` does in the usual
 *           implementations.
 * \~spanish NULL si no se pudo.  Con @p n a cero devuelve un bloque valido de
 *           tamano minimo, como hace `malloc` en las implementaciones normales.
 * \~
 *
 * \~english
 * @code
 *   // Plugging the project's allocator into Capstone.
 *   cs_opt_mem mem;
 *   mem.malloc   = vesta_host_alloc;
 *   mem.calloc   = vesta_host_calloc;
 *   mem.realloc  = vesta_host_realloc;
 *   mem.free     = vesta_host_free;
 *   mem.vsnprintf = vsnprintf;
 *   cs_option(0, CS_OPT_MEM, (size_t)&mem);
 *   // From here on, what Capstone allocates shows in our counters.
 * @endcode
 *
 * \~spanish
 * @code
 *   // Enchufar el asignador del proyecto a Capstone.
 *   cs_opt_mem mem;
 *   mem.malloc   = vesta_host_alloc;
 *   mem.calloc   = vesta_host_calloc;
 *   mem.realloc  = vesta_host_realloc;
 *   mem.free     = vesta_host_free;
 *   mem.vsnprintf = vsnprintf;
 *   cs_option(0, CS_OPT_MEM, (size_t)&mem);
 *   // A partir de aqui, lo que reserve Capstone sale en nuestros contadores.
 * @endcode
 *
 * \~
 */
void *vesta_host_alloc(size_t n);

/**
 * @brief
 * \~english Serves @p count elements of @p size bytes, ZEROED.
 * \~spanish Sirve @p count elementos de @p size bytes, PUESTOS A CERO.
 * \~
 *
 * \~english
 * @par Threads
 * **Safe from any thread**, the same as @c vesta_host_alloc.
 *
 * \~spanish
 * @par Hilos
 * **Segura desde cualquier hilo**, igual que @c vesta_host_alloc.
 *
 * \~
 * @param count
 * \~english how many elements are wanted.
 * \~spanish cuantos elementos se quieren.
 * \~
 * @param size
 * \~english how many bytes each of them takes.
 * \~spanish cuantos bytes ocupa cada uno.
 * \~
 * @return
 * \~english NULL if it could not be done, or if the product would overflow.
 * \~spanish NULL si no se pudo o si el producto se desbordaria.
 * \~
 *
 * \~english
 * @code
 *   int *v = (int *)vesta_host_calloc(n, sizeof(int));   // n zeroed ints
 *   if (v == NULL) return -1;
 * @endcode
 *
 * \~spanish
 * @code
 *   int *v = (int *)vesta_host_calloc(n, sizeof(int));   // n enteros a cero
 *   if (v == NULL) return -1;
 * @endcode
 *
 * \~
 */
void *vesta_host_calloc(size_t count, size_t size);

/**
 * @brief
 * \~english Changes the size of @p p to @p n bytes.  The equivalent of
 *          `realloc`.
 * \~spanish Cambia el tamano de @p p a @p n bytes.  Equivalente a `realloc`.
 * \~
 *
 * \~english
 * With @p p at NULL it is the same as @c vesta_host_alloc.  With @p n at zero
 * it frees and returns NULL.
 *
 * The old size does NOT have to be passed in: from a pointer its chunk comes
 * out with a mask, and from the chunk its size class.  That is why only what is
 * needed gets copied.
 *
 * @par Threads
 * **Safe from any thread.**  It works even if @p p was allocated by ANOTHER
 * one.
 *
 * \~spanish
 * Con @p p a NULL equivale a @c vesta_host_alloc.  Con @p n a cero libera y
 * devuelve NULL.
 *
 * El tamano viejo NO hace falta pasarlo: de un puntero se saca su trozo con una
 * mascara, y del trozo su clase.  Por eso se puede copiar lo justo.
 *
 * @par Hilos
 * **Segura desde cualquier hilo.**  Vale aunque @p p lo reservara OTRO hilo.
 *
 * \~
 * @param p
 * \~english the block to resize, or NULL.
 * \~spanish el bloque a redimensionar, o NULL.
 * \~
 * @param n
 * \~english the new size in bytes.
 * \~spanish el nuevo tamano en bytes.
 * \~
 * @return
 * \~english NULL if it could not be done; in that case @p p IS STILL VALID and
 *           has to be freed, just as `realloc` dictates.
 * \~spanish NULL si no se pudo; en ese caso @p p SIGUE SIENDO VALIDO y hay que
 *           liberarlo, igual que manda `realloc`.
 * \~
 *
 * \~english
 * @code
 *   char *buf = (char *)vesta_host_alloc(64);
 *   char *more = (char *)vesta_host_realloc(buf, 256);
 *   if (more == NULL) { vesta_host_free(buf); return -1; }  // buf still alive
 *   buf = more;
 * @endcode
 *
 * \~spanish
 * @code
 *   char *buf = (char *)vesta_host_alloc(64);
 *   char *mas = (char *)vesta_host_realloc(buf, 256);
 *   if (mas == NULL) { vesta_host_free(buf); return -1; }  // buf sigue vivo
 *   buf = mas;
 * @endcode
 *
 * \~
 */
void *vesta_host_realloc(void *p, size_t n);

/**
 * @brief
 * \~english Gives a block back.  The equivalent of `free`.  With NULL it does
 *          nothing.
 * \~spanish Devuelve un bloque.  Equivalente a `free`.  Con NULL no hace nada.
 * \~
 *
 * \~english
 * @par Threads
 * **Safe from any thread**, even if another one allocated it: in that case the
 * block goes onto an atomic stack belonging to its owner, blocking nobody.
 *
 * \~spanish
 * @par Hilos
 * **Segura desde cualquier hilo**, incluso si lo reservo otro: en ese caso el
 * bloque va a una pila atomica de su dueño, sin bloquear a nadie.
 *
 * \~
 * @param p
 * \~english the block to give back, or NULL.
 * \~spanish el bloque a devolver, o NULL.
 * \~
 *
 * \~english
 * @code
 *   vesta_host_free(buf);
 * @endcode
 *
 * \~spanish
 * @code
 *   vesta_host_free(buf);
 * @endcode
 *
 * \~
 */
void vesta_host_free(void *p);

/**
 * @brief
 * \~english How much the block @p p really measures.
 * \~spanish Cuanto mide de verdad el bloque @p p.
 * \~
 *
 * \~english
 * It serves for what other libraries call `malloc_usable_size`: it allows the
 * rounding to be used instead of wasted.
 *
 * @par Threads
 * **Safe from any thread.**  It only reads the chunk header.
 *
 * \~spanish
 * Sirve para lo que en otras librerias se llama `malloc_usable_size`: permite
 * aprovechar el redondeo en vez de desperdiciarlo.
 *
 * @par Hilos
 * **Segura desde cualquier hilo.**  Solo lee la cabecera del trozo.
 *
 * \~
 * @param p
 * \~english the block to measure.
 * \~spanish el bloque a medir.
 * \~
 * @return
 * \~english the USABLE bytes, which may be more than the ones asked for
 *           because the size is rounded up to a class.  Zero if @p p is not
 *           ours.
 * \~spanish los bytes UTILIZABLES, que pueden ser mas de los pedidos porque el
 *           tamano se redondea a una clase.  Cero si @p p no es nuestro.
 * \~
 *
 * \~english
 * @code
 *   // Grow without asking for anything if the rounding was already enough.
 *   if (vesta_host_usable_size(buf) >= needed) { ... }
 * @endcode
 *
 * \~spanish
 * @code
 *   // Crecer sin pedir nada si el redondeo ya daba de sobra.
 *   if (vesta_host_usable_size(buf) >= necesito) { ... }
 * @endcode
 *
 * \~
 */
size_t vesta_host_usable_size(const void *p);

/**
 * @brief
 * \~english States what THIS thread is allocating for, until it is taken off.
 * \~spanish Declara para que se reserva en ESTE hilo, hasta que se quite.
 * \~
 *
 * \~english
 * It is the C equivalent of `util::AllocScope`.  In C there are no destructors,
 * so restoring is the caller's business -- and it has to be done, because a tag
 * left in place contaminates everything that thread allocates afterwards, and
 * that is not noticed by looking: the split comes out plausible but wrong.
 *
 * @par Threads
 * **Safe from any thread**, and it affects ONLY the caller.  If the work is
 * handed to other threads, the tag does not travel on its own.
 *
 * \~spanish
 * Es el equivalente en C de `util::AllocScope`.  En C no hay destructores, asi
 * que restaurar es cosa del que llama -- y hay que hacerlo, porque una etiqueta
 * que se queda puesta contamina todo lo que ese hilo reserve despues, y eso no
 * se nota mirando: el reparto sale plausible pero equivocado.
 *
 * @par Hilos
 * **Segura desde cualquier hilo**, y afecta SOLO al que llama.  Si el trabajo
 * se reparte a otros hilos, la etiqueta no viaja sola.
 *
 * \~
 * @param use
 * \~english a `VestaAllocUse`: @c VestaUse_Unknown, @c VestaUse_Instant,
 *           @c VestaUse_Medium or @c VestaUse_Long.
 * \~spanish un `VestaAllocUse`: @c VestaUse_Unknown, @c VestaUse_Instant,
 *           @c VestaUse_Medium o @c VestaUse_Long.
 * \~
 * @param shape
 * \~english a `VestaAllocShape`: @c VestaShape_Unknown, @c VestaShape_Fixed or
 *           @c VestaShape_Growing.
 * \~spanish un `VestaAllocShape`: @c VestaShape_Unknown, @c VestaShape_Fixed o
 *           @c VestaShape_Growing.
 * \~
 * @return
 * \~english the tag that was in place, to put it back afterwards.
 * \~spanish la etiqueta que habia puesta, para devolverla despues.
 * \~
 *
 * \~english
 * @code
 *   // What the decompression allocates counts as instant and fixed.
 *   const unsigned before =
 *       vesta_host_push_tag(VestaUse_Instant, VestaShape_Fixed);
 *   decompress(...);
 *   vesta_host_pop_tag(before);
 * @endcode
 *
 * \~spanish
 * @code
 *   // Lo que reserve la descompresion se cuenta como instantaneo y fijo.
 *   const unsigned antes =
 *       vesta_host_push_tag(VestaUse_Instant, VestaShape_Fixed);
 *   descomprimir(...);
 *   vesta_host_pop_tag(antes);
 * @endcode
 *
 * \~
 */
unsigned vesta_host_push_tag(unsigned use, unsigned shape);

/**
 * @brief
 * \~english Puts back the tag that @c vesta_host_push_tag returned.
 * \~spanish Vuelve a poner la etiqueta que devolvio @c vesta_host_push_tag.
 * \~
 *
 * \~english
 * @par Threads
 * **Safe from any thread**, and it affects only the caller.
 *
 * \~spanish
 * @par Hilos
 * **Segura desde cualquier hilo**, y afecta solo al que llama.
 *
 * \~
 * @param previous
 * \~english the tag that @c vesta_host_push_tag handed back.
 * \~spanish la etiqueta que devolvio @c vesta_host_push_tag.
 * \~
 */
void vesta_host_pop_tag(unsigned previous);

/**
 * @brief
 * \~english Declares how much of what is allocated next will be TOUCHED.
 * \~spanish Declara cuanto se va a TOCAR de lo que se reserve a continuacion.
 * \~
 *
 * \~english
 * The C side of `util::AllocFillScope`, and the same deal as
 * @c vesta_host_push_tag: there are no destructors in C, so putting it back is
 * the caller's job -- and it has to be done, because one left in place taints
 * everything that thread allocates afterwards.
 *
 * WHAT IT BUYS.  A big zeroed block has two opposite right answers -- keep it
 * and clear it, or ask the system for a fresh one that arrives zeroed -- and
 * they cross at a FRACTION of the block, not at a size, so the allocator cannot
 * choose on its own.  Declaring sparse on a block that really is read sparsely
 * is worth 1.14x to 2.37x against the C runtime on the sizes measured;
 * declaring it on one that is read whole costs about as much the other way.
 * It is an assertion, and it only ever costs speed.
 *
 * @par Threads
 * **Safe from any thread**, and it affects ONLY the caller.  If the work is
 * handed to other threads, what was declared does not travel on its own.
 *
 * \~spanish
 * El lado en C de `util::AllocFillScope`, y el mismo trato que
 * @c vesta_host_push_tag: en C no hay destructores, asi que devolverlo es cosa
 * del que llama -- y hay que hacerlo, porque uno que se queda puesto contamina
 * todo lo que ese hilo reserve despues.
 *
 * QUE COMPRA.  Un bloque grande a cero tiene dos respuestas correctas y
 * opuestas -- quedarselo y limpiarlo, o pedirle uno fresco al sistema que llega
 * ya a cero -- y se cruzan en una FRACCION del bloque, no en un tamano, asi que
 * el asignador no puede elegir solo.  Declarar disperso en un bloque que de
 * verdad se lee a trozos vale de 1,14x a 2,37x contra la libreria de C en los
 * tamanos medidos; declararlo en uno que se lee entero cuesta mas o menos lo
 * mismo en el otro sentido.  Es una afirmacion, y solo cuesta velocidad.
 *
 * @par Hilos
 * **Segura desde cualquier hilo**, y afecta SOLO al que llama.  Si el trabajo
 * se reparte a otros hilos, lo declarado no viaja solo.
 *
 * \~
 * @param fill
 * \~english a `VestaAllocFill`: @c VestaFill_Unknown, @c VestaFill_Sparse,
 *           @c VestaFill_Dense or @c VestaFill_All.  Out of range is trimmed
 *           rather than refused: the worst that happens is the allocator picks
 *           the path it would have picked with nothing said.
 * \~spanish un `VestaAllocFill`: @c VestaFill_Unknown, @c VestaFill_Sparse,
 *           @c VestaFill_Dense o @c VestaFill_All.  Fuera de rango se recorta en
 *           vez de rechazarse: lo peor que pasa es que el asignador elija el
 *           camino que habria elegido sin que nadie dijera nada.
 * \~
 * @return
 * \~english what was in place, to put it back afterwards.
 * \~spanish lo que habia puesto, para devolverlo despues.
 * \~
 *
 * \~english
 * @code
 *   // The hash table will only ever touch the buckets it hashes to.
 *   const unsigned before = vesta_host_push_fill(VestaFill_Sparse);
 *   void *table = vesta_host_calloc(1, 8u << 20);
 *   vesta_host_pop_fill(before);
 * @endcode
 *
 * \~spanish
 * @code
 *   // La tabla hash solo tocara los cubos donde caiga.
 *   const unsigned antes = vesta_host_push_fill(VestaFill_Sparse);
 *   void *tabla = vesta_host_calloc(1, 8u << 20);
 *   vesta_host_pop_fill(antes);
 * @endcode
 *
 * \~
 */
unsigned vesta_host_push_fill(unsigned fill);

/**
 * @brief
 * \~english Puts back what @c vesta_host_push_fill returned.
 * \~spanish Vuelve a poner lo que devolvio @c vesta_host_push_fill.
 * \~
 * @param previous
 * \~english what that call returned.
 * \~spanish lo que devolvio aquella llamada.
 * \~
 */
void vesta_host_pop_fill(unsigned previous);

/* ---------------------------------------------------------------------------
 *  Los contadores  --  The counters
 * ------------------------------------------------------------------------- */

/**
 * @brief
 * \~english How much the process has allocated, and what for.
 * \~spanish Cuanto y de que ha reservado el proceso.
 * \~
 *
 * \~english
 * ONE SINGLE DEFINITION.  It is declared here, where C can read it, and the C++
 * side names it again with `using`.  Declaring it twice -- once per language --
 * is the kind of duplication nobody sees break: the day a counter is added to
 * one, the other still compiles and starts reading a different structure.
 *
 * The two sizes it carries inside are macros in
 * `util/report/alloc_sites_c.h` for the same reason, and the C++ side CHECKS
 * that they match its `constexpr`: if somebody changes one and not the other,
 * it does not compile.
 *
 * \~spanish
 * UNA SOLA DEFINICION.  Se declara aqui, donde C puede leerla, y el lado de
 * C++ la nombra otra vez con `using`.  Declararla dos veces -- una por
 * lenguaje -- es la duplicacion que nadie ve romperse: el dia que se le anade
 * un contador a una, la otra sigue compilando y empieza a leer una estructura
 * distinta.
 *
 * Los dos tamanos que lleva dentro estan como macros en
 * `util/report/alloc_sites_c.h` por el mismo motivo, y el lado de C++ COMPRUEBA que
 * coinciden con sus `constexpr`: si alguien cambia uno y no el otro, no
 * compila.
 *
 * \~
 */
typedef struct VestaHostAllocStats {
    /**
     * \~english
     * Small allocations, split BY TAG.  Indexed directly by the packed tag,
     * with nothing to take apart: counting by purpose is then the SAME indexed
     * increment that counted the total, not one more.  That is why there is no
     * separate total counter -- it would be the sum of this one -- and why
     * keeping the split costs not one instruction on the hot path.
     *
     * \~spanish
     * Reservas pequenas, repartidas POR ETIQUETA.  Indexado directamente por
     * la etiqueta empaquetada, sin descomponer nada: contar por proposito es
     * entonces el MISMO incremento indexado que contaba el total, no uno mas.
     * Por eso no hay un contador de totales aparte -- seria la suma de esto --
     * y por eso llevar el reparto no cuesta ni una instruccion en el camino
     * caliente.
     *
     * \~
     */
    uint64_t by_tag[VESTA_ALLOC_TAG_SLOTS];
    /// \~english the sum of `by_tag`; whoever asks fills it in
    /// \~spanish suma de `by_tag`; la rellena el que pide  \~
    uint64_t small_allocs;
    /// \~english frees done by the owning thread
    /// \~spanish liberaciones del propio hilo  \~
    uint64_t small_frees;
    /// \~english frees done by ANOTHER thread
    /// \~spanish liberaciones hechas por OTRO hilo  \~
    uint64_t remote_frees;
    /// \~english large allocations, served as spans
    /// \~spanish reservas grandes, servidas por tramos  \~
    uint64_t large_allocs;
    /// \~english spans given back
    /// \~spanish tramos devueltos  \~
    uint64_t large_frees;
    /// \~english chunks asked of the region
    /// \~spanish trozos pedidos a la region  \~
    uint64_t chunks;
    /// \~english bytes committed out of the region
    /// \~spanish bytes comprometidos de la region  \~
    uint64_t bytes_reserved;
    /**
     * \~english
     * Times an owner id was asked for and there was none left.
     *
     * Other than zero means there were more owners ALIVE at once than the cap,
     * and that those threads served themselves from the SHARED lists, behind
     * the allocator's one lock.  It is not a failure -- it works all the same
     * -- but it is the only way to find out: from outside, all that shows is
     * that everything runs slower for no visible reason.
     *
     * \~spanish
     * Veces que se pidio identificador de dueño y no quedaba.
     *
     * Distinto de cero significa que hubo mas dueños VIVOS a la vez que el
     * tope, y que esos hilos se sirvieron de las listas COMPARTIDAS, detras
     * del unico cerrojo del asignador.  No es un fallo -- funciona igual --,
     * pero es la unica forma de enterarse: desde fuera solo se nota que todo
     * va mas lento sin razon visible.
     *
     * \~
     */
    uint64_t no_owner_id;
    /**
     * \~english
     * The split of the sizes ASKED FOR.  The exact ranges live in
     * `util/alloc/size_buckets.h`.  They reach all the way up because the TAIL
     * has to be visible to decide how to serve the large allocations.  Only
     * filled in with `VESTA_HOST_ALLOC_STATS=1`.
     *
     * \~spanish
     * Reparto de los tamanos PEDIDOS.  Los tramos exactos viven en
     * `util/alloc/size_buckets.h`.  Llegan hasta arriba porque hay que poder ver la
     * COLA para decidir como servir las reservas grandes.  Solo se llena con
     * `VESTA_HOST_ALLOC_STATS=1`.
     *
     * \~
     */
    uint64_t size_hist[VESTA_ALLOC_SIZE_BUCKETS];
} VestaHostAllocStats;

/**
 * @brief
 * \~english Copies the process counters into @p out.
 * \~spanish Copia los contadores del proceso en @p out.
 * \~
 *
 * \~english
 * The threads are added up when they are asked for, so that counting does not
 * force any synchronisation on the fast path.
 *
 * \~spanish
 * Se suman los hilos al pedirlos, para que contarlos no obligue a sincronizar
 * en el camino rapido.
 *
 * \~
 * @param out
 * \~english where to leave them; it is overwritten whole.
 * \~spanish donde dejarlos; se sobrescribe entero.
 * \~
 *
 * \~english
 * @code
 *   VestaHostAllocStats s;
 *   vesta_host_stats(&s);
 *   printf("chunks=%llu region=%llu KiB\n",
 *          (unsigned long long)s.chunks,
 *          (unsigned long long)(s.bytes_reserved / 1024));
 * @endcode
 *
 * \~spanish
 * @code
 *   VestaHostAllocStats s;
 *   vesta_host_stats(&s);
 *   printf("trozos=%llu region=%llu KiB\n",
 *          (unsigned long long)s.chunks,
 *          (unsigned long long)(s.bytes_reserved / 1024));
 * @endcode
 *
 * \~
 */
void vesta_host_stats(VestaHostAllocStats *out);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // VESTA_UTIL_HOST_ALLOCATOR_C_H

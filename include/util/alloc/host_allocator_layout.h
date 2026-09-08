/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/alloc/host_allocator_layout.h
 * @brief
 * \~english How the allocator's memory is laid out: region, chunks and classes.
 * \~spanish Como esta puesta la memoria del asignador: region, trozos y clases.
 * \~
 *
 * \~english
 * WHY IT IS SEPARATE FROM THE ALLOCATOR.  This is not the allocator, it is its
 * GEOMETRY, and more than one party is interested in it:
 *
 *   - the allocator proper (`util/alloc/host_allocator.h`);
 *   - the bump arena, which takes chunks from the same region with a different
 *     magic -- that way freeing knows which of the two a pointer belongs to
 *     WITHOUT searching any table and without one new instruction on the path;
 *   - the parallel metadata of the diagnostic mode, which index by chunk and by
 *     block and need the same arithmetic;
 *   - the C layer for third-party libraries.
 *
 * With the geometry in one place, all of them use THE SAME arithmetic.  Spread
 * out, the first time somebody changes the chunk size one of the four goes
 * quietly out of step.
 *
 * THERE IS NO ALLOCATION CODE HERE: only constants and the four functions that
 * translate a pointer into its place.  All of them are inline, branch free and
 * touch no more than one cache line.
 *
 * ------------------------------------------------------------------------
 * THE RULE FOR THE CONSTANTS IN THIS FILE
 * ------------------------------------------------------------------------
 *
 * Almost all of them come out of MEASURING today's compiler.  That is fine --
 * it is how tuning works -- but it has a failure mode of its own and it has to
 * be avoided on purpose:
 *
 *   **a constant tuned with today's measurement must not become a limit on
 *   what can be done tomorrow.**
 *
 * This allocator does not serve the compiler alone: it is meant to be reused --
 * the arena, the diagnostic mode, third-party libraries through the C layer --
 * and each of those asks for different things.  So, for every constant, one has
 * to know which of these two groups it is in:
 *
 *  - **Tuning**: changing it moves performance and nothing else.  They are
 *    `kSizes` (and with it `kMaxSmall`), `kChunkBytes` and `kMaxSpanChunks`.
 *    Outside their good range the allocator **still serves**, only worse.
 *  - **Capacity**: going past it changes BEHAVIOUR.  Today one is left,
 *    `kMaxThreads`: a thread beyond the cap is without lists of its own
 *    forever.  `kRegionBytes` stopped being one -- it is now ASKED FOR and
 *    whatever the system grants is accepted -- and the span one is not either:
 *    going past it only means that block is not held back when released.
 *
 * And the rule that covers both: **if a bound is crossed, it is COUNTED and
 * said** (`g_gave_up` in the `.cpp`).  A bound crossed quietly stops being a
 * tuning knob and becomes a failure nobody sees.
 *
 * \~spanish
 * POR QUE ESTA SEPARADO DEL ASIGNADOR.  Esto no es el asignador, es su
 * GEOMETRIA, y hay mas de un interesado en ella:
 *
 *   - el asignador propiamente dicho (`util/alloc/host_allocator.h`);
 *   - la arena de golpe, que toma trozos de la misma region con otra marca --
 *     asi liberar sabe de cual de los dos es un puntero SIN buscar en ninguna
 *     tabla y sin ninguna instruccion nueva en el camino;
 *   - los metadatos en paralelo del modo de diagnostico, que indexan por trozo
 *     y por bloque y necesitan las mismas cuentas;
 *   - la capa en C para las librerias de terceros.
 *
 * Con la geometria en un sitio, todos usan LAS MISMAS cuentas.  Repartida, la
 * primera vez que alguien cambie el tamano de trozo se descuadra en silencio
 * uno de los cuatro.
 *
 * NO HAY CODIGO DE ASIGNAR AQUI: solo constantes y las cuatro funciones que
 * traducen un puntero a su sitio.  Todas son en linea, sin ramas y sin tocar
 * mas de una linea de cache.
 *
 * ------------------------------------------------------------------------
 * REGLA PARA LAS CONSTANTES DE ESTE FICHERO
 * ------------------------------------------------------------------------
 *
 * Casi todas salen de MEDIR el compilador de hoy.  Eso esta bien -- es como se
 * afina --, pero tiene un modo de fallo propio y hay que evitarlo a proposito:
 *
 *   **una constante afinada con la medida de hoy no puede convertirse en un
 *   limite de lo que se puede hacer manana.**
 *
 * Este asignador no sirve solo al compilador: esta pensado para reusarse -- la
 * arena, el modo de diagnostico, las librerias de terceros por la capa en C --
 * y cada uno de esos pide cosas distintas.  Asi que, para cada constante, hay
 * que saber en cual de estos dos grupos esta:
 *
 *  - **De afinado**: cambiarla mueve el rendimiento y nada mas.  Son
 *    `kSizes` (y con ella `kMaxSmall`), `kChunkBytes` y `kMaxSpanChunks`.  Fuera
 *    de su rango bueno el asignador **sigue sirviendo**, solo que peor.
 *  - **De capacidad**: pasarse cambia el COMPORTAMIENTO.  Hoy queda una,
 *    `kMaxThreads`: un hilo por encima del tope se queda sin listas propias
 *    para siempre.  `kRegionBytes` dejo de serlo -- ahora se PIDE y se acepta
 *    lo que el sistema conceda --, y la de tramos tampoco lo es: pasarse solo
 *    significa que ese bloque no se guarda al soltarlo.
 *
 * Y la regla que las cubre a las dos: **si una cota se cruza, se CUENTA y se
 * dice** (`g_gave_up` en el `.cpp`).  Una cota que se cruza en silencio deja de
 * ser un afinado y se convierte en un fallo que nadie ve.
 *
 * \~
 */
#ifndef VESTA_UTIL_HOST_ALLOCATOR_LAYOUT_H
#define VESTA_UTIL_HOST_ALLOCATOR_LAYOUT_H

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace util {

/**
 * @brief
 * \~english The largest allocation the allocator serves; above it goes to the
 *          system.
 * \~spanish Mayor reserva que sirve el asignador; por encima va al sistema.
 * \~
 *
 * \~english
 * Raised from 1 KiB to 2 KiB with the numbers up front.  With the cap at 1 KiB,
 * 420,219 allocations went to the system and cost 0.476 s between `malloc` and
 * `free` -- **1.13 us per pair** -- while the 43.9 million we served ran at 5.5
 * ns.  Every large allocation cost some two hundred times what a small one did.
 *
 * WHY 2 KiB AND NOT MORE.  Raising the cap has a price in memory, because this
 * allocator does NOT give anything back to the system while the system's
 * `malloc` usually does give large blocks back.  The whole curve, measured:
 *
 *     cap     time      peak memory
 *     1 KiB   ref.      779 MB
 *     2 KiB   -3.2%     788 MB   (+1.1%)
 *     4 KiB   -4.7%     836 MB   (+7.3%)
 *     8 KiB   a draw    837 MB   (+7.4%)
 *
 * 2 KiB gives almost all the gain for a seventh of the cost.  And from 8 KiB up
 * there is no gain at all, which is what one would expect: past the 4 KiB page
 * what is asked for is whole pages, and cutting those into classes inside a
 * chunk only adds waste -- the system already knows how to hand out pages.
 *
 * ------------------------------------------------------------------------
 * AND WHY NOT 64 KiB, GIVEN THAT THERE IS ROOM
 * ------------------------------------------------------------------------
 *
 * With the big region (`kBigChunkBytes`) the classes from 16 to 64 KiB DO fit:
 * 64 KiB gives fifteen blocks in a 1 MiB chunk, and in a 64 KiB one not even
 * a single one fitted.  It was tried, and **it does not stay**.  Measured with
 * `bench_vs_malloc`, the same on Windows and on Linux (the Linux figures here,
 * ns per operation):
 *
 *     case            16 KiB    64 KiB
 *     hot 64K           5.96      2.34     <- 2.5x better
 *     burst 64K        19.61     10.36     <- 1.9x better
 *     churn 64K         8.03      3.58     <- 2.2x better
 *     grow to 64K      29.38     92.85     <- 3.2x WORSE, and loses to glibc
 *     grow to 1M       22.52    135.68     <- 6x WORSE, and loses to glibc
 *
 * The reason is not the rounding: a class block **cannot be stretched in
 * place** and a span can (`try_extend_span`).  On raising the cap, every
 * doubling between 16 and 64 KiB stopped being stretched and started being
 * copied -- which is exactly what a container that grows does.  Trading two
 * wins for two losses against the system is not tuning, it is a regression.
 *
 * WHAT IT WOULD TAKE to have them.  Telling an allocation that GROWS apart from
 * one that does not, and sending the growing one to a span even when it fits a
 * class.  That axis is already planned -- it is `AllocShape` in
 * `util/alloc/alloc_tag.h` -- so it is routing work and not geometry.  Until
 * then, 16 KiB.
 *
 * \~spanish
 * Subido de 1 KiB a 2 KiB con la cuenta delante.  Con el tope en 1 KiB, 420.219
 * reservas se iban al sistema y costaban 0,476 s entre `malloc` y `free` --
 * **1,13 us por par** --, mientras las 43,9 millones que serviamos nosotros
 * iban a 5,5 ns.  Cada reserva grande costaba unas doscientas veces lo que una
 * pequena.
 *
 * POR QUE 2 KiB Y NO MAS.  Subir el tope tiene un precio en memoria, porque
 * este asignador NO devuelve nada al sistema mientras que el `malloc` del
 * sistema si suele devolver los bloques grandes.  Medida la curva entera:
 *
 *     tope    tiempo    pico de memoria
 *     1 KiB   ref.      779 MB
 *     2 KiB   -3,2%     788 MB   (+1,1%)
 *     4 KiB   -4,7%     836 MB   (+7,3%)
 *     8 KiB   empate    837 MB   (+7,4%)
 *
 * 2 KiB da casi toda la ganancia por la septima parte del coste.  Y de 8 KiB
 * hacia arriba ya no hay ganancia ninguna, que es lo esperable: pasada la
 * pagina de 4 KiB lo que se pide son paginas completas, y trocearlas en clases
 * dentro de un trozo solo anade desperdicio -- el sistema ya sabe entregar
 * paginas.
 *
 * ------------------------------------------------------------------------
 * Y POR QUE NO 64 KiB, HABIENDO SITIO
 * ------------------------------------------------------------------------
 *
 * Con la region grande (`kBigChunkBytes`) las clases de 16 a 64 KiB SI caben:
 * 64 KiB da quince bloques en un trozo de 1 MiB, y en uno de 64 KiB no cabia ni
 * uno.  Se probo, y **no se queda**.  Medido con `bench_vs_malloc`, igual en
 * Windows y en Linux (aqui las cifras de Linux, ns por operacion):
 *
 *     caso            16 KiB    64 KiB
 *     hot 64K           5,96      2,34     <- 2,5x mejor
 *     burst 64K        19,61     10,36     <- 1,9x mejor
 *     churn 64K         8,03      3,58     <- 2,2x mejor
 *     grow to 64K      29,38     92,85     <- 3,2x PEOR, y pierde con glibc
 *     grow to 1M       22,52    135,68     <- 6x PEOR, y pierde con glibc
 *
 * La razon no es el redondeo: un bloque de clase **no se puede estirar en el
 * sitio** y un tramo si (`try_extend_span`).  Al subir el tope, cada duplicacion
 * entre 16 y 64 KiB dejo de estirarse y paso a copiarse -- que es justo lo que
 * hace un contenedor que crece --.  Cambiar dos victorias por dos derrotas
 * contra el sistema no es un afinado, es una regresion.
 *
 * QUE HARIA FALTA para tenerlas.  Distinguir la reserva que CRECE de la que no,
 * y mandar la que crece a un tramo aunque quepa en una clase.  Ese eje ya esta
 * previsto -- es `AllocShape` de `util/alloc/alloc_tag.h` --, asi que es trabajo de
 * encaminamiento y no de geometria.  Hasta entonces, 16 KiB.
 *
 * \~
 */
inline constexpr size_t kMaxSmall = 16384;

/// \~english The alignment `operator new` guarantees for any type.
/// \~spanish Alineacion que garantiza `operator new` para cualquier tipo.
/// \~
inline constexpr size_t kAlign = 16;

/**
 * @brief
 * \~english The size classes.
 * \~spanish Clases de tamano.
 * \~
 *
 * \~english
 * A 16-byte step at the bottom and a coarser one further up: what gets asked
 * for most are small objects (a 32-byte pair, a table node), where wasting 16
 * bytes is already a lot, while up top the allocations are rare and the
 * relative waste is smaller.  The largest internal waste stays at 14%.
 *
 * \~spanish
 * Paso de 16 bytes abajo y mas grueso arriba: lo que mas se pide son objetos
 * pequenos (un par de 32 bytes, un nodo de tabla), donde desperdiciar 16 bytes
 * ya es mucho, mientras que arriba las reservas son raras y el desperdicio
 * relativo es menor.  El mayor desperdicio interno queda en el 14%.
 *
 * \~
 */
inline constexpr uint32_t kSizes[] = {
    16, 32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512,
    640, 768, 896, 1024,
    // De 1 KiB a 2 KiB el paso se abre un poco: son el 0,5% de las reservas,
    // asi que el desperdicio por redondeo pesa poco.  El mayor queda en un
    // 15% (1.537 bytes van a una clase de 1.792).
    1152, 1280, 1408, 1536, 1792, 2048,
    /* DE 2 KiB A 16 KiB: el hueco que costaba caro.  Sin estas clases, una
     * peticion de 4 KiB se iba al camino de TRAMOS y se llevaba un trozo
     * ENTERO de 64 KiB -- dieciseis veces el desperdicio -- ademas de tomar el
     * cerrojo de tramos.  Medido contra `malloc`, era el unico sitio donde
     * perdiamos de largo: `burst` de 4 KiB, 16,9 ns frente a 9,0.
     *
     * Con clase propia, un trozo de 64 KiB da quince bloques de 4 KiB y ninguno
     * pasa por el cerrojo.
     *
     * OJO CON SUBIR MAS.  Arriba del todo quedan pocos bloques por trozo -- a
     * 16 KiB salen tres --, y un trozo no se devuelve hasta que se vacia
     * entero, asi que pasado cierto punto se cambia tiempo por fragmentacion.
     * 16 KiB es donde estaba el equilibrio al medirlo; ver el banco
     * `bench_vs_malloc`, que es lo que hay que volver a mirar antes de tocar
     * esta lista. */
    2560, 3072, 3584, 4096, 5120, 6144, 8192, 10240, 12288, 16384};
/// \~english How many size classes there are.
/// \~spanish Cuantas clases de tamano hay.
/// \~
inline constexpr uint32_t kClasses = sizeof(kSizes) / sizeof(kSizes[0]);

/**
 * \~english
 * @brief How many blocks the hot batch holds.
 *
 * WHAT THE BATCH IS FOR.  A free list is a pointer chase: to know the new head
 * you must first load the block's own first word, so consecutive allocations of
 * one class form a chain of dependent L1 loads and the machine cannot start the
 * next one early.  Measured with the microarchitecture counters, that shows up
 * as L1 latency dependency with nothing above L1 involved at all.
 *
 * The batch breaks the chain for a run of allocations in the SAME class: the
 * blocks are handed out from an array by index, which depends on nothing the
 * previous allocation loaded.  Freeing into it is also one store to the array
 * instead of a store INTO the block, which is a line the caller may not have
 * touched in a while.
 *
 * EIGHT, so the array is exactly one cache line.  Bigger buys a longer run
 * between refills and costs the same line count in every cache -- and there are
 * `kTotalCaches` of them, so this is not free memory.  One class at a time and
 * not one batch per class for the same reason: per class it would be
 * `kClasses` lines each, which multiplies the whole table.
 *
 * \~spanish
 * @brief Cuantos bloques guarda el lote caliente.
 *
 * PARA QUE ESTA.  Una lista libre es una persecucion de punteros: para saber la
 * nueva cabeza hay que cargar antes la primera palabra del propio bloque, asi
 * que reservas seguidas de una clase forman una cadena de cargas dependientes
 * de L1 y la maquina no puede adelantar la siguiente.  Medido con los
 * contadores microarquitectonicos, eso sale como dependencia de latencia de L1
 * sin que L2 ni nada por encima participe.
 *
 * El lote rompe la cadena para una tirada de reservas de la MISMA clase: los
 * bloques salen de un array por indice, que no depende de nada que cargara la
 * reserva anterior.  Liberar en el es tambien un almacen al array en vez de un
 * almacen DENTRO del bloque, que es una linea que el llamante puede llevar rato
 * sin tocar.
 *
 * OCHO, para que el array sea exactamente una linea de cache.  Mas grande
 * compra una tirada mas larga entre recargas y cuesta las mismas lineas en cada
 * cache -- y hay `kTotalCaches` de ellos, asi que esta memoria no es gratis --.
 * Una clase cada vez y no un lote por clase por lo mismo: por clase serian
 * `kClasses` lineas cada uno, que multiplica la tabla entera.
 * \~
 */
inline constexpr uint32_t kBatchSlots = 8;

/**
 * \~english
 * @brief The largest block the hot batch takes charge of.
 *
 * NOT EVERY SIZE WANTS THE SAME MECHANISM, and measuring says so plainly.  The
 * batch wins where a block is taken and given back over and over -- it comes
 * out of the array and goes back into it without anybody touching its memory --
 * and that is what small allocations do.  In a BURST, where hundreds are taken
 * and only then given back, it breaks no chain at all: the list gets walked
 * either way, and the batch just adds a refill every eight.
 *
 * Measured, per size, worst pattern against best:
 *
 *       16 B     every pattern gains
 *       64 B     every pattern gains
 *      256 B     `hot` 1.33 -> 0.87 but `burst` 1.71 -> 2.10 and
 *                `calloc` 3.02 -> 3.75, and those are 23% and 24% against
 *
 * So the batch stops here and the classes above keep the plain list, which is
 * the same thing this library does everywhere else: specialise, rather than
 * make one mechanism serve sizes it was not good for.  It also happens to be
 * where the volume is -- 81% of allocations in a real compilation are 64 bytes
 * or less.
 *
 * IT COSTS THE FAST PATH NOTHING, and that is why the check is not there:
 * `batch_cls` only ever holds a class at or below this one, so `batch_cls == k`
 * already answers "is this a batched class?".  The only place that has to know
 * is @c pop_block_refill, which runs once every @c kBatchSlots allocations.
 *
 * \~spanish
 * @brief El bloque mas grande del que se hace cargo el lote caliente.
 *
 * NO TODOS LOS TAMANOS QUIEREN EL MISMO MECANISMO, y medir lo dice sin rodeos.
 * El lote gana donde un bloque se coge y se devuelve una y otra vez -- sale del
 * array y vuelve a el sin que nadie toque su memoria --, que es lo que hacen
 * las reservas pequenas.  En una RAFAGA, donde se cogen cientos y solo despues
 * se devuelven, no rompe ninguna cadena: la lista se recorre igual y el lote
 * solo anade una recarga cada ocho.
 *
 * Medido, por tamano, el peor patron contra el mejor:
 *
 *       16 B     ganan todos los patrones
 *       64 B     ganan todos los patrones
 *      256 B     `hot` 1,33 -> 0,87 pero `burst` 1,71 -> 2,10 y
 *                `calloc` 3,02 -> 3,75, que son un 23% y un 24% en contra
 *
 * Asi que el lote se para aqui y las clases de arriba siguen con la lista de
 * siempre, que es lo mismo que esta libreria hace en todo lo demas:
 * especializar, en vez de hacer que un mecanismo sirva tamanos para los que no
 * era bueno.  Y ademas es donde esta el volumen: el 81% de las reservas de una
 * compilacion real son de 64 bytes o menos.
 *
 * AL CAMINO RAPIDO NO LE CUESTA NADA, y por eso la comprobacion no esta ahi:
 * `batch_cls` solo llega a contener clases de esta para abajo, asi que
 * `batch_cls == k` ya responde "es una clase con lote?".  El unico sitio que
 * tiene que saberlo es @c pop_block_refill, que corre una vez cada
 * @c kBatchSlots reservas.
 * \~
 */
inline constexpr size_t kBatchMaxSize = 128;

/// \~english The class index @c kBatchMaxSize falls in, worked out from the
///           table so that changing one does not silently disagree with the
///           other.
/// \~spanish El indice de clase en que cae @c kBatchMaxSize, sacado de la tabla
///           para que cambiar uno no discrepe en silencio con el otro.
/// \~
inline constexpr uint32_t batch_max_class() noexcept {
    uint32_t last = 0;
    for (uint32_t i = 0; i < kClasses; ++i)
        if (kSizes[i] <= kBatchMaxSize) last = i;
    return last;
}
inline constexpr uint32_t kBatchMaxClass = batch_max_class();

/// \~english The chunk asked of the region every time a class runs out of
///           blocks.
/// \~spanish Trozo que se pide a la region cada vez que una clase se queda sin
///           bloques.
/// \~
inline constexpr size_t kChunkBytes = 64 * 1024;

/**
 * @brief
 * \~english The virtual region reserved in one go.
 * \~spanish Region virtual que se reserva de una vez.
 * \~
 *
 * \~english
 * A range of ADDRESSES is booked and real memory is only handed over chunk by
 * chunk as it is needed.
 *
 * WHY SO BIG.  Because running out of region does not give an error, it gives a
 * SILENT DEGRADATION: from that point on every allocation goes to the system
 * allocator and the only thing anyone notices is that it runs slower.  It was
 * at 1 GiB, and compiling 144k lines already committed 760.9 MiB -- 74% -- so a
 * slightly larger project crossed the limit without warning.
 *
 * WHY NOT BIGGER STILL.  Because booking is NOT free, however often that is
 * said: Windows charges for the page bookkeeping.  Measured on this machine:
 *
 *     reserve     time      working set   commit
 *      16 GiB       10 us       +84 KiB    +32 KiB
 *      64 GiB       23 us      +184 KiB   +160 KiB
 *     256 GiB      104 us      +572 KiB   +676 KiB
 *       1 TiB      459 us      +2.1 MiB   +2.7 MiB
 *       4 TiB    2,195 us      +8.3 MiB  +10.9 MiB
 *      16 TiB    8,517 us     +32.8 MiB  +43.8 MiB
 *
 * That works out at ~2.7 MiB of commit and ~0.5 ms per TiB, and those
 * milliseconds land on STARTUP, which is already something this project chases.
 *
 * 256 GiB is where the two things cross: 104 us and half a megabyte, and **340
 * times** the observed peak.  Past that point the limit stops being the address
 * space and becomes PHYSICAL memory -- the allocator never gives chunks back,
 * so the region accumulates the historical peak, and exhausting 256 GiB means
 * the machine ran out of memory long before.  No allocator saves anything
 * there, so reserving more buys nothing.
 *
 * And that is why several regions are NOT needed, which is what would have
 * forced @c in_region to stop being two compares.
 *
 * On 32 bits that much does not fit, and there one does have to be modest.
 *
 * \~spanish
 * Se apalabra un rango de DIRECCIONES y solo se entrega memoria de verdad
 * trozo a trozo segun hace falta.
 *
 * POR QUE TAN GRANDE.  Porque quedarse sin region no da un error, da una
 * DEGRADACION MUDA: a partir de ese punto todas las reservas se van al
 * asignador del sistema y lo unico que se nota es que va mas lento.  Estuvo en
 * 1 GiB, y compilar 144k lineas ya comprometia 760,9 MiB -- el 74% --, asi que
 * un proyecto algo mayor cruzaba el limite sin avisar.
 *
 * POR QUE NO MAS GRANDE TODAVIA.  Porque apalabrar NO es gratis, aunque se diga
 * mucho: Windows cobra la contabilidad de paginas.  Medido en esta maquina:
 *
 *     reserva     tiempo    working set   commit
 *      16 GiB       10 us       +84 KiB    +32 KiB
 *      64 GiB       23 us      +184 KiB   +160 KiB
 *     256 GiB      104 us      +572 KiB   +676 KiB
 *       1 TiB      459 us      +2,1 MiB   +2,7 MiB
 *       4 TiB    2.195 us      +8,3 MiB  +10,9 MiB
 *      16 TiB    8.517 us     +32,8 MiB  +43,8 MiB
 *
 * Sale a ~2,7 MiB de commit y ~0,5 ms por TiB, y esos milisegundos caen en el
 * ARRANQUE, que ya es de por si algo que este proyecto persigue.
 *
 * 256 GiB es donde se cruzan las dos cosas: 104 us y medio mega, y **340 veces**
 * el pico observado.  Pasado ese punto el limite deja de ser el espacio de
 * direcciones y pasa a ser la memoria FISICA -- el asignador no devuelve trozos
 * nunca, asi que la region acumula el pico historico, y agotar 256 GiB
 * significa que la maquina se quedo sin memoria mucho antes.  Ahi no hay
 * asignador que salve nada, asi que reservar mas no compra nada.
 *
 * Y por eso NO hacen falta varias regiones, que es lo que habria obligado a que
 * @c in_region dejara de ser dos comparaciones.
 *
 * En 32 bits no cabe tanto, y ahi si hay que ser modesto.
 *
 * \~
 */
inline constexpr size_t kRegionBytes =
    sizeof(void *) >= 8 ? (size_t(256) << 30)  // 256 GiB
                        : (size_t(256) << 20); // 256 MiB

/**
 * @brief
 * \~english Below this it is not worth setting the region up at all.
 * \~spanish Por debajo de esto no merece la pena montar la region.
 * \~
 *
 * \~english
 * @c kRegionBytes is what is ASKED FOR, not what is obtained: the system is
 * asked halving the amount until it goes through (`util::os_reserve_largest`),
 * so on a tight machine one ends up with less and carries on working.  This is
 * the floor: 64 MiB holds a thousand chunks, which is already enough for the
 * allocator to contribute something; below that, better to be honest and not
 * set it up.
 *
 * \~spanish
 * @c kRegionBytes es lo que se PIDE, no lo que se consigue: se pregunta al
 * sistema bajando a la mitad hasta que entre (`util::os_reserve_largest`), asi
 * que en una maquina apretada se acaba con menos y se sigue funcionando.  Este
 * es el suelo: con 64 MiB caben mil trozos, que ya da para que el asignador
 * aporte algo; por debajo, mejor ser sincero y no montarlo.
 *
 * \~
 */
inline constexpr size_t kRegionMinBytes = size_t(64) << 20; // 64 MiB

/// \~english How many chunks fit in the region.  Whoever indexes BY chunk needs
///           it.
/// \~spanish Cuantos trozos caben en la region.  Lo necesita quien indexe POR
///           trozo.
/// \~
inline constexpr size_t kMaxChunks = kRegionBytes / kChunkBytes;

/**
 * @brief
 * \~english The chunk of the BIG classes, which is not the same as the one for
 *          the small ones and therefore lives in a region of its own.
 * \~spanish El trozo de las clases GRANDES, que no es el mismo que el de las
 *          pequenas y por eso vive en su propia region.
 * \~
 *
 * \~english
 * WHY ANOTHER SIZE.  A 64 KiB chunk with the header up front leaves
 * `65536 - 16` useful bytes, and that does not divide the powers of two well:
 * the 16 KiB class gets THREE blocks and leaves 16,368 bytes dead, 25% of the
 * chunk, held for as long as a single block lives.  With 8 KiB it is 12.5% and
 * with 4 KiB 6.2%.  It is not theoretical -- `bench_vs_malloc` shows it in its
 * memory table.
 *
 * With a 1 MiB chunk the same arithmetic gives 63 blocks and **1.56%**, and it
 * also makes a 64 KiB class viable, which does not fit even once in a 64 KiB
 * chunk.
 *
 * WHY A SEPARATE REGION AND NOT THE SAME PLACE.  Because a pointer is resolved
 * by MASKING, and the mask depends on the chunk size: with both things mixed in
 * one range, the free path would have to work out which one it is BEFORE
 * masking, and the small allocations -- which are 81% -- would pay for that
 * too.  With two ranges, the new check falls inside the "not from the small
 * region" branch, which a small block NEVER reaches: its free path does not
 * change by one instruction.  Checked by disassembling, not assumed.
 *
 * It is the same idea that governs the whole allocator: a need of ONE class of
 * allocations is met by specialising, not by changing what all of them use.
 *
 * \~spanish
 * POR QUE OTRO TAMANO.  Un trozo de 64 KiB con la cabecera delante deja
 * `65536 - 16` bytes utiles, y eso no divide bien las potencias de dos: la
 * clase de 16 KiB saca TRES bloques y deja 16.368 bytes muertos, un 25% del
 * trozo, retenidos mientras viva un solo bloque.  Con 8 KiB es el 12,5% y con
 * 4 KiB el 6,2%.  No es teorico -- lo saca `bench_vs_malloc` en su tabla de
 * memoria --.
 *
 * Con un trozo de 1 MiB la misma cuenta da 63 bloques y **un 1,56%**, y ademas
 * hace viable una clase de 64 KiB, que en un trozo de 64 KiB no cabe ni una
 * vez.
 *
 * POR QUE UNA REGION APARTE Y NO EL MISMO SITIO.  Porque un puntero se resuelve
 * ENMASCARANDO, y la mascara depende del tamano de trozo: con las dos cosas
 * mezcladas en un rango, el camino de liberar tendria que averiguar de cual es
 * ANTES de enmascarar, y eso lo pagarian tambien las reservas pequenas -- que
 * son el 81% --.  Con dos rangos, la comprobacion nueva cae dentro de la rama
 * "no es de la region pequena", a la que un bloque pequeno NUNCA llega: su
 * camino de liberacion no cambia ni una instruccion.  Comprobado desensamblando,
 * no supuesto.
 *
 * Es la misma idea que gobierna todo el asignador: una necesidad de UNA clase
 * de reservas se resuelve especializando, no cambiando lo que usan todas.
 *
 * \~
 */
inline constexpr size_t kBigChunkBytes = size_t(1) << 20; // 1 MiB

/**
 * @brief
 * \~english From which class size onwards it is served from the big region.
 * \~spanish Desde que tamano de clase se sirve de la region grande.
 * \~
 *
 * \~english
 * Below 2 KiB the waste of a 64 KiB chunk stays at 3% and almost always under
 * 1.5%, so moving those would buy nothing and would instead commit 1 MiB per
 * class and per thread.  From 2 KiB up the waste starts to climb and does not
 * stop.
 *
 * \~spanish
 * Por debajo de 2 KiB el desperdicio de un trozo de 64 KiB se queda en el 3% y
 * casi siempre por debajo del 1,5%, asi que mudarlas no compraria nada y en
 * cambio comprometeria 1 MiB por clase y por hilo.  Desde 2 KiB el desperdicio
 * empieza a subir y no para.
 *
 * \~
 */
inline constexpr size_t kBigClassMin = 2048;

/// \~english Addresses booked for the big classes.  Far less than the small
///           region because these allocations are rare -- 0.9% -- and still
///           plenty: 16,384 chunks of 1 MiB.
/// \~spanish Direcciones que se apalabran para las clases grandes.  Mucho menos
///           que la region pequena porque estas reservas son raras -- el 0,9%
///           --, y aun asi sobrado: son 16.384 trozos de 1 MiB.
/// \~
inline constexpr size_t kBigRegionBytes = size_t(16) << 30;   // 16 GiB
/// \~english The floor for the big region, the same idea as
///           @c kRegionMinBytes.
/// \~spanish El suelo de la region grande, la misma idea que
///           @c kRegionMinBytes.
/// \~
inline constexpr size_t kBigRegionMinBytes = size_t(32) << 20; // 32 MiB

/**
 * @brief
 * \~english The cap on cache ids: how many distinct owners a chunk header can
 *          name.
 * \~spanish Tope de identificadores de cache: cuantos dueños distintos puede
 *          nombrar la cabecera de un trozo.
 * \~
 *
 * \~english
 * It is NOT "live threads".  A thread asks for its id the first time it
 * allocates and gives it back when it dies (see the recycling in the `.cpp`),
 * so what this bounds are the SIMULTANEOUS owners.  It used not to be given
 * back and the cap was on threads that had ever existed, which is a very
 * different thing: a program that creates and destroys threads exhausted it
 * without ever having more than a handful at a time.
 *
 * GOING PAST IT IS NOT "off to the system", which is what used to be written
 * here.  The thread is left without lists of its own and starts being served
 * from the SHARED ones, which are behind the allocator's one lock; from then on
 * every allocation of every overflowed thread serialises there.  Measured with
 * 24 threads, the same binary: 1.73 ns per operation with ids to spare, 447.86
 * without them.
 *
 * \~spanish
 * NO es "hilos vivos".  Un hilo pide su identificador la primera vez que
 * reserva y lo devuelve al morir (ver el reciclado en el `.cpp`), asi que lo
 * que esto limita son los duenos SIMULTANEOS.  Antes no se devolvia y el tope
 * era de hilos que hubieran existido alguna vez, que es otra cosa muy distinta:
 * un programa que crea y destruye hilos lo agotaba sin tener nunca mas de un
 * punado a la vez.
 *
 * PASARSE NO ES "irse al sistema", que es lo que ponia aqui.  El hilo se queda
 * sin listas propias y pasa a servirse de las COMPARTIDAS, que estan detras del
 * unico cerrojo del asignador; a partir de ahi todas las reservas de todos los
 * hilos desbordados se serializan ahi.  Medido con 24 hilos, mismo binario:
 * 1,73 ns por operacion con identificadores de sobra, 447,86 sin ellos.
 *
 * \~
 */
inline constexpr uint32_t kMaxThreads = 64;

/**
 * @brief
 * \~english Owner ids reserved for the LOCK-FREE per-thread policy.
 * \~spanish Identificadores de dueño reservados para la politica por hilo SIN
 *          CERROJOS.
 * \~
 *
 * \~english
 * TWO POLICIES, TWO BOUNDS.  The ids above bound MEMORY: no matter how many
 * threads a program starts, the shared allocator never uses more than
 * `kMaxThreads` caches, and whoever does not get one is served from the shared
 * lists behind a lock.  These ids bound LATENCY instead: a thread using
 * @c PerThreadAllocator always gets a cache of its own, so it never touches a
 * lock, and the price is paid in memory -- one cache per live thread.
 *
 * Neither is better; they bound different things, which is why both exist.
 *
 * The ranges are kept apart on purpose.  Sharing one pool would let the
 * lock-free policy eat the ids the shared one relies on, and the memory bound
 * that is the whole point of the shared policy would quietly stop holding.
 *
 * The tables live in `.bss`, so this costs address space and commit charge,
 * not resident memory: a page is only touched when a thread actually takes the
 * cache that lives in it.
 *
 * \~spanish
 * DOS POLITICAS, DOS COTAS.  Los identificadores de arriba acotan la MEMORIA:
 * por muchos hilos que arranque un programa, el asignador compartido no usa
 * nunca mas de `kMaxThreads` caches, y quien no consiga uno se sirve de las
 * listas compartidas detras de un cerrojo.  Estos acotan en cambio la LATENCIA:
 * un hilo que use @c PerThreadAllocator recibe siempre un cache propio, asi que
 * no toca ningun cerrojo, y el precio se paga en memoria -- un cache por hilo
 * vivo.
 *
 * Ninguna es mejor; acotan cosas distintas, que es por lo que existen las dos.
 *
 * Los rangos se mantienen separados a proposito.  Compartir un solo reparto
 * dejaria que la politica sin cerrojos se comiera los identificadores en los
 * que se apoya la compartida, y la cota de memoria que es toda la razon de ser
 * de la compartida dejaria de cumplirse en silencio.
 *
 * Las tablas viven en `.bss`, asi que esto cuesta espacio de direcciones y
 * cargo de compromiso, no memoria residente: una pagina solo se toca cuando un
 * hilo se lleva de verdad el cache que vive en ella.
 *
 * \~
 */
inline constexpr uint32_t kPerThreadCaches = 960;

/// \~english Entries in the cache and remote tables: both policies index the
///           same arrays.
/// \~spanish Entradas de las tablas de caches y de remotos: las dos politicas
///           indexan los mismos arrays.
/// \~
inline constexpr uint32_t kTotalCaches = kMaxThreads + kPerThreadCaches;

static_assert(kPerThreadCaches % 64 == 0,
              "the free-id map is an array of 64-bit words, so the count has "
              "to fill whole words: a partial word would hand out ids that do "
              "not exist");

/// \~english The magic of a chunk cut into classes, the ordinary allocator's.
/// \~spanish Marca de un trozo troceado en clases, el del asignador normal.
/// \~
inline constexpr uint32_t kChunkMagic = 0x56455354u; // 'VEST'

/**
 * @brief
 * \~english The magic of a SPAN: several chunks in a row that are ONE single
 *          allocation.
 * \~spanish Marca de un TRAMO: varios trozos seguidos que son UNA sola reserva.
 * \~
 *
 * \~english
 * It is how the large allocations are served without asking anybody for them.
 * The chunk is still the addressing unit -- a pointer's header is located by
 * masking, one instruction -- and a span is no more than N chunks in a row
 * whose header is in the first one.  Since the user is always handed
 * `head + sizeof(ChunkHeader)`, `chunk_of()` lands on the head alone and the
 * free path does not change.
 *
 * \~spanish
 * Es como se sirven las reservas grandes sin pedirselas a nadie.  El trozo
 * sigue siendo la unidad de direccionamiento -- la cabecera de un puntero se
 * localiza enmascarando, una instruccion --, y un tramo no es mas que N trozos
 * seguidos cuya cabecera esta en el primero.  Como al usuario se le entrega
 * siempre `cabeza + sizeof(ChunkHeader)`, `chunk_of()` cae en la cabeza sola y
 * el camino de liberar no cambia.
 *
 * \~
 */
inline constexpr uint32_t kSpanMagic = 0x5350414eu; // 'SPAN'

/**
 * @brief
 * \~english Up to what size a span is HELD BACK for reuse, in chunks.
 * \~spanish Hasta que tamano se GUARDA un tramo para reusarlo, en trozos.
 * \~
 *
 * \~english
 * **It is not a cap on what can be asked for.**  A larger allocation is served
 * all the same; the only thing that changes is that on release it goes back to
 * the system instead of being kept, because holding a block of over 16 MiB in
 * case it is needed again costs more than it saves.
 *
 * 256 chunks are 16 MiB, which is where the tail measured TODAY ends: 17
 * allocations of a whole build fall between 1 and 16 MiB and none above.
 * Tomorrow it may be another figure; that is why it is a cache bound and not a
 * condition for working.
 *
 * \~spanish
 * **No es un tope de lo que se puede pedir.**  Una reserva mayor se sirve
 * igual; lo unico que cambia es que al soltarla se devuelve al sistema en vez
 * de quedarsela, porque guardar un bloque de mas de 16 MiB por si vuelve a
 * hacer falta cuesta mas de lo que ahorra.
 *
 * 256 trozos son 16 MiB, que es donde acaba la cola medida HOY: 17 reservas de
 * toda una compilacion caen entre 1 y 16 MiB y ninguna por encima.  Manana
 * puede ser otra cifra; por eso es una cota de cache y no una condicion de
 * funcionamiento.
 *
 * \~
 */
inline constexpr uint32_t kMaxSpanChunks = 256;

/**
 * @brief
 * \~english How many span sizes each thread holds back for itself, with no
 *          lock.
 * \~spanish Cuantos tamanos de tramo se guarda cada hilo para si, sin cerrojo.
 * \~
 *
 * \~english
 * WHY.  The span path takes a global lock to allocate and another to free, and
 * that is ALMOST ALL of what it costs: measured, an allocate/free pair of 64
 * KiB runs at 6.27 ns and the lock alone is 4.75 -- **76%**.  It was the only
 * place in the allocator where the fast path synchronised, and also the only
 * one where we lost against the system (`churn 64K`, 0.87x).
 *
 * With one span held back per thread and per size, freeing and asking again for
 * the same size -- which is what a recycled buffer does -- touches neither the
 * lock nor the shared lists.  It is the same thing the allocator already does
 * with small blocks, applied where it was missing.
 *
 * HOW MANY SIZES IT COVERS, and why it is not two any more.  It used to be two
 * -- spans of one and two chunks, up to 128 KiB -- and the reason given was
 * that "90% of the large allocations land there according to the measured
 * split".  That split was Vesta's.  This is a separate library, and the sizes
 * one consumer asks for say nothing about the next one: code that works on
 * images, network buffers or matrices lives entirely above that line, and for
 * it the fast path did not exist at all.
 *
 * What that cost, measured with 24 threads on a profile with 8% spans: **72.5%
 * of all CPU time spinning on the span lock**, against 6.8% doing the actual
 * work.  And the span path stopped scaling completely -- cost per operation per
 * core went from 10.3 ns on one thread to 302 ns on 24, and with a
 * span-dominated profile from 38.5 to 2505, where adding threads makes the
 * whole thing 3.8x SLOWER than running single-threaded.
 *
 * Overridable, because it is a property of the CONSUMER and not of this
 * library: it decides the largest span that can ever take the fast path, and
 * how much of `ThreadCache` is spent on the table.
 *
 * @see kSpanCacheBytes, which is what actually bounds the memory.
 *
 * \~spanish
 * POR QUE.  El camino de tramos toma un cerrojo global para reservar y otro
 * para soltar, y eso es CASI TODO lo que cuesta: medido, un par
 * reservar/soltar de 64 KiB va a 6,27 ns y solo el cerrojo son 4,75 -- el
 * **76%** --.  Era el unico sitio del asignador donde el camino rapido
 * sincronizaba, y tambien el unico donde perdiamos contra el sistema
 * (`churn 64K`, 0,87x).
 *
 * Con un tramo guardado por hilo y por tamano, soltar y volver a pedir el mismo
 * tamano -- que es lo que hace un bufer que se recicla -- no toca el cerrojo ni
 * las listas compartidas.  Es lo mismo que el asignador ya hace con los
 * bloques pequenos, aplicado donde faltaba.
 *
 * CUANTOS TAMANOS CUBRE, y por que ya no son dos.  Eran dos -- tramos de uno y
 * de dos trozos, hasta 128 KiB -- y la razon que se daba era que "el 90% de las
 * reservas grandes caen ahi segun el reparto medido".  Ese reparto era el de
 * Vesta.  Esto es una libreria aparte, y los tamanos que pide un consumidor no
 * dicen nada del siguiente: codigo que trabaja con imagenes, buferes de red o
 * matrices vive entero por encima de esa linea, y para el el camino rapido
 * sencillamente no existia.
 *
 * Lo que costaba, medido con 24 hilos sobre un perfil con un 8% de tramos: **el
 * 72,5% de todo el tiempo de CPU girando en el cerrojo de tramos**, frente a un
 * 6,8% haciendo el trabajo de verdad.  Y el camino de tramos dejaba de escalar
 * del todo -- el coste por operacion y por nucleo pasaba de 10,3 ns con un hilo
 * a 302 con 24, y con un perfil dominado por tramos de 38,5 a 2.505, donde
 * anadir hilos deja el conjunto 3,8x MAS LENTO que en un solo hilo.
 *
 * Se puede cambiar, porque es una propiedad del CONSUMIDOR y no de esta
 * libreria: decide el tramo mayor que puede llegar a tomar el camino rapido, y
 * cuanto del `ThreadCache` se gasta en la tabla.
 *
 * @see kSpanCacheBytes, que es lo que de verdad acota la memoria.
 *
 * \~
 *
 * \~english
 * @code
 *   // Never allocates over 256 KiB, and wants the table small:
 *   //   -DVESTA_ALLOC_SPAN_CACHE_SLOTS=4
 * @endcode
 *
 * \~spanish
 * @code
 *   // No reserva nunca por encima de 256 KiB, y quiere la tabla pequena:
 *   //   -DVESTA_ALLOC_SPAN_CACHE_SLOTS=4
 * @endcode
 *
 * \~
 */
#ifndef VESTA_ALLOC_SPAN_CACHE_SLOTS
#define VESTA_ALLOC_SPAN_CACHE_SLOTS 32
#endif
inline constexpr uint32_t kSpanCacheSlots = VESTA_ALLOC_SPAN_CACHE_SLOTS;

/**
 * @defgroup span_policy How the shared span pool synchronises
 * @brief
 * \~english Four mechanisms for the same job, chosen at compile time.
 * \~spanish Cuatro mecanismos para el mismo trabajo, elegidos al compilar.
 * \~
 *
 * \~english
 * WHY THERE IS A CHOICE HERE AT ALL.  Measured with 24 threads, **60% of all
 * CPU time was spent spinning on the span lock** even after the per-thread span
 * cache took half the traffic away.  The lock is not needed to hand a span out
 * or to take one back -- it is needed because spans get SPLIT and MERGED, and
 * those reach into a second span and into a doubly-linked list at once.
 *
 * And that is the opening: splitting and coalescing exist only to fight
 * fragmentation, so a coalesce that cannot happen right now can be SKIPPED and
 * the result is still correct -- just a little more fragmented.  It is the only
 * place in this allocator where the operation that needs exclusion is one you
 * are allowed to give up on.
 *
 * Which of these wins is not something to reason about from first principles;
 * it depends on how often each path is actually taken, so all four are built
 * from the same source and measured with the same benchmark.
 *
 * \~spanish
 * POR QUE HAY UNA ELECCION AQUI.  Medido con 24 hilos, **el 60% de todo el
 * tiempo de CPU se iba girando en el cerrojo de tramos** incluso despues de que
 * el cache de tramos por hilo se llevara la mitad del trafico.  El cerrojo no
 * hace falta para entregar un tramo ni para recogerlo -- hace falta porque los
 * tramos se PARTEN y se FUNDEN, y eso alcanza a un segundo tramo y a una lista
 * doblemente enlazada a la vez.
 *
 * Y ahi esta el hueco: partir y fundir existen solo para pelear contra la
 * fragmentacion, asi que una fusion que no se puede hacer ahora mismo se puede
 * SALTAR y el resultado sigue siendo correcto -- solo un poco mas fragmentado
 * --.  Es el unico sitio de este asignador donde la operacion que necesita
 * exclusion es una a la que esta permitido renunciar.
 *
 * Cual de estas gana no es algo que se razone desde los principios; depende de
 * cuantas veces se toma de verdad cada camino, asi que las cuatro se construyen
 * del mismo fuente y se miden con el mismo banco.
 *
 * \~
 * @{
 */
/// \~english Today's: one spin lock around the lists, the splitting and the
///           coalescing.
/// \~spanish La de hoy: un cerrojo de giro alrededor de las listas, del partir
///           y del fundir.
/// \~
#define VESTA_SPAN_LOCKED 0
/// \~english Lock-free lists; the lock is left only for coalescing, which
///           reaches out.
/// \~spanish Listas sin cerrojos; el cerrojo se queda solo para fundir, que
///           alcanza fuera.
/// \~
#define VESTA_SPAN_HYBRID 1
/// \~english Lock-free lists and no coalescing on the free path; done later, in
///           batch.
/// \~spanish Listas sin cerrojos y sin fundir al liberar; se hace despues, por
///           tandas.
/// \~
#define VESTA_SPAN_DEFERRED 2
/// \~english No lock anywhere: the neighbour is claimed with a CAS, or given up
///           on.  NOT FINISHED -- see the guard below before reaching for it.
/// \~spanish Sin cerrojo en ningun sitio: el vecino se reclama con un CAS, o se
///           renuncia.  SIN TERMINAR -- ver la guarda de abajo antes de
///           echarle mano.
/// \~
#define VESTA_SPAN_LOCKFREE 3

#ifndef VESTA_ALLOC_SPAN_POLICY
#define VESTA_ALLOC_SPAN_POLICY VESTA_SPAN_HYBRID
#endif

/*
 * THE LOCK-FREE POLICY IS WRITTEN AND IT IS NOT CORRECT.  It refuses to build
 * on purpose, because the alternative is worse: it passes single-threaded, so
 * it would go through anybody's quick check and corrupt memory later, under
 * load, with nothing pointing back here.
 *
 * WHAT IS WRONG, kept because the diagnosis was the hard part.  A span that
 * gets absorbed stays behind as a leftover entry in its stack, and its slot in
 * `g_span_next` is still part of that chain.  If that same chunk is later handed
 * out, freed, and pushed onto a DIFFERENT stack, the slot is overwritten and
 * the first chain starts following a link that belongs to the second: the two
 * stacks cross.
 *
 * A chunk owns ONE link slot and can be in TWO stacks at once -- as a real
 * entry and as a leftover.  Moving the link out of the span made READING it
 * safe, which was the earlier problem and is genuinely fixed; it does not stop
 * the slot being REUSED.  That needs a chunk not to be reused while a leftover
 * of it is still reachable, which is deferred reclamation -- epochs or hazard
 * pointers -- and that does not exist yet.
 *
 * `test_span_race` catches it: it hangs.
 */
#if VESTA_ALLOC_SPAN_POLICY == VESTA_SPAN_LOCKFREE
#error "VESTA_SPAN_LOCKFREE is not finished: a chunk can sit in two stacks at \
once -- once for real and once as the leftover of an absorption -- and they \
share a single link slot, so reusing the chunk crosses the two chains. It \
needs deferred reclamation (epochs or hazard pointers), which is not written. \
It passes single-threaded and hangs under `test_span_race`, so it would slip \
through a quick check. Use VESTA_SPAN_HYBRID or VESTA_SPAN_DEFERRED."
#endif

/**
 * @brief
 * \~english True for the policies that park freed spans out of reach.
 * \~spanish Cierto para las politicas que aparcan los tramos liberados donde no
 *          se llega.
 * \~
 *
 * \~english
 * Two of the four do: they buy their speed by putting a freed span where no
 * coalescer can find it, and pay for it later.  The locked one has nowhere to
 * park -- every free goes straight to the pool -- and the lock-free one needs
 * nowhere, because its pool costs no lock to reach in the first place.
 *
 * \~spanish
 * Dos de las cuatro lo hacen: compran su velocidad poniendo un tramo liberado
 * donde ningun fusionador puede encontrarlo, y lo pagan despues.  La de cerrojo
 * no tiene donde aparcar -- toda liberacion va derecha al reparto -- y la sin
 * cerrojos no lo necesita, porque llegar a su reparto no cuesta ningun cerrojo
 * de entrada.
 *
 * \~
 */
#define VESTA_SPAN_HAS_PARKING                                                 \
    (VESTA_ALLOC_SPAN_POLICY == VESTA_SPAN_HYBRID ||                           \
     VESTA_ALLOC_SPAN_POLICY == VESTA_SPAN_DEFERRED)
/** @} */

/**
 * @brief
 * \~english Whether the size histogram is compiled in at all.
 * \~spanish Si el histograma de tamanos llega siquiera a compilarse.
 * \~
 *
 * \~english
 * It is gated at RUN time by an environment flag, which means the check itself
 * -- a load of a global and a branch -- is paid on every allocation of every
 * build, to decide not to do something.  This is the switch that removes the
 * check too, for a build that will never be asked for the histogram.
 *
 * On by default: the figures are worth more than the instructions to most
 * people, and turning them off silently would make a report that says nothing
 * look like a program that allocates nothing.
 *
 * \~spanish
 * Se gobierna en EJECUCION con una variable de entorno, lo que significa que la
 * comprobacion misma -- una carga de un global y una rama -- se paga en todas
 * las reservas de todas las compilaciones, para decidir no hacer algo.  Este es
 * el interruptor que quita tambien la comprobacion, para una compilacion a la
 * que nunca se le va a pedir el histograma.
 *
 * Encendido por defecto: las cifras valen mas que las instrucciones para casi
 * todo el mundo, y apagarlas en silencio haria que un informe que no dice nada
 * pareciera un programa que no reserva nada.
 *
 * \~
 */
#ifndef VESTA_ALLOC_SIZE_HISTOGRAM
#define VESTA_ALLOC_SIZE_HISTOGRAM 1
#endif

/**
 * @brief
 * \~english Whether the thread's cache gets a thread variable of its own.
 * \~spanish Si el cache del hilo recibe una variable de hilo propia.
 * \~
 *
 * \~english
 * The general thread slot costs one more DEPENDENT load than reading a thread
 * variable directly -- the slot index, which then indexes the array -- and on
 * this path that is what counts: it is bound by the latency of loads that
 * depend on each other, not by how many instructions there are.  Measured on
 * ELF: -4.9% geomean over hot and churn, and -14% on the best case.
 *
 * NEVER ON WINDOWS, and the guard below enforces it rather than trusting
 * anybody to remember.  There `__thread` is MinGW's EMULATED thread storage,
 * where every access is a call to `__emutls_get_address`: measured on this
 * toolchain at 10.83 ns against 0.85 for the direct segment read, twelve times
 * worse -- on a path that serves a block in about 13 ns, so looking up the
 * state would cost as much as the work.  And it is not only speed: emulated
 * thread storage has already HUNG this process once, through the guard variable
 * a dynamically-initialised one generates.
 *
 * That is the whole reason @c util::ThreadSlot exists, and on Windows it stays
 * the way in.  `__thread` inside `thread_slot.h` is not a precedent for using
 * it here: it is under the same kind of guard, for the platform where it is
 * real thread storage.
 *
 * \~spanish
 * La ranura por hilo general cuesta una carga DEPENDIENTE mas que leer una
 * variable de hilo directamente -- el indice de ranura, que luego indexa el
 * array -- y en este camino eso es lo que cuenta: lo limita la latencia de
 * cargas que dependen unas de otras, no cuantas instrucciones haya.  Medido en
 * ELF: -4,9% de media geometrica entre `hot` y `churn`, y -14% en el mejor
 * caso.
 *
 * NUNCA EN WINDOWS, y la guarda de abajo lo impone en vez de fiarse de que
 * alguien se acuerde.  Ahi `__thread` es el almacenamiento por hilo EMULADO de
 * MinGW, donde cada acceso es una llamada a `__emutls_get_address`: medido en
 * esta cadena de herramientas, 10,83 ns frente a 0,85 de la lectura directa del
 * segmento, doce veces peor -- en un camino que sirve un bloque en unos 13 ns,
 * asi que localizar el estado costaria tanto como el trabajo --.  Y no es solo
 * velocidad: el almacenamiento por hilo emulado ya COLGO este proceso una vez,
 * por la variable de guarda que genera uno de inicializacion dinamica.
 *
 * Esa es toda la razon de ser de @c util::ThreadSlot, y en Windows sigue siendo
 * la puerta.  El `__thread` de dentro de `thread_slot.h` no es un precedente
 * para usarlo aqui: esta bajo la misma clase de guarda, para la plataforma
 * donde es almacenamiento por hilo de verdad.
 *
 * \~
 */
#if defined(_WIN32)
#ifdef VESTA_ALLOC_DIRECT_CACHE_TLS
#undef VESTA_ALLOC_DIRECT_CACHE_TLS
#endif
#define VESTA_ALLOC_DIRECT_CACHE_TLS 0
#elif !defined(VESTA_ALLOC_DIRECT_CACHE_TLS)
#define VESTA_ALLOC_DIRECT_CACHE_TLS 1
#endif

/**
 * @brief
 * \~english From which size a recycled block is zeroed by the SYSTEM, not by
 *          us.
 * \~spanish Desde que tamano un bloque reciclado lo pone a cero el SISTEMA y no
 *          nosotros.
 * \~
 *
 * \~spanish
 * DOS FORMAS DE ENTREGAR UN BLOQUE A CERO, y escalan distinto.  Una pasada de
 * `memset` cuesta en proporcion al TAMANO y se paga mire o no mire quien llama
 * esa memoria.  Devolver las paginas y volver a cogerlas cuesta una llamada al
 * sistema mas un fallo por cada pagina que quien llama toca DE VERDAD -- asi
 * que sigue al uso en vez de al tamano.
 *
 * SEGUN EL SISTEMA, y no por poco: en uno compensa a partir de cuatro mebibytes
 * y en el otro no compensa nunca.  Medido con la misma llamada que hace el
 * asignador -- diciendole al relleno cual es la alineacion -- frente a devolver
 * las paginas y volver a cogerlas, segun cuanto del bloque toque luego quien
 * llama:
 *
 *     Linux                 1/64         1/8         todo
 *       1 MiB        sistema 2,66x  nuestro 1,14x  nuestro 7,81x
 *       4 MiB        sistema 8,11x  sistema 1,75x  nuestro 3,98x  <- desde aqui
 *      16 MiB       sistema 20,01x  sistema 3,46x  nuestro 2,29x
 *
 *     Windows               1/64         1/8         todo
 *       4 MiB        sistema 1,97x  nuestro 1,69x nuestro 10,95x
 *       8 MiB        sistema 2,05x  nuestro 1,55x nuestro 12,85x
 *      16 MiB        sistema 4,99x  sistema 1,07x  nuestro 6,60x  <- sigue
 *                                                                    perdiendo
 *
 * Y EL RIESGO NO ES SIMETRICO, que es lo que decide la respuesta.  Quien llena
 * el bloque paga la diferencia entera, y este lado del asignador no tiene forma
 * de saber que clase de llamante tiene -- asi que el liston es que el mejor
 * caso valga MAS de lo que cuesta el peor.
 *
 * APAGADO POR DEFECTO EN LOS DOS, Y LA TABLA DE ARRIBA ES LA RAZON DE QUE
 * PAREZCA QUE NO DEBERIA ESTARLO.  Esas cifras salen de un banco del mecanismo
 * a solas, y ese banco estaba equivocado justo en lo que decide: su "llamante"
 * tocaba un byte por pagina, mientras que uno que LEE el bloque lee TODOS los
 * bytes, de paginas que acaban de fallar.  Medido sobre lo de verdad a 8 MiB,
 * el orden se da la vuelta:
 *
 *     8 MiB, Linux      lo pone a cero el sistema   lo ponemos nosotros
 *       lee 1/64                          27.786                105.480
 *                                                        sistema 3,9x
 *       lo lee todo                    1.699.725                232.295
 *                                                        nuestro 7,3x
 *
 * Asi que el mismo tamano es casi cuatro veces mejor o siete veces peor segun
 * nada mas que lo que haga luego quien llama -- que es exactamente lo que un
 * umbral por TAMANO no puede saber --.  Encenderlo seria apostar el caso que
 * llena para ganar el disperso, y el caso que llena pierde por mas.
 *
 * El mecanismo se queda, y este interruptor tambien, porque para quien SABE que
 * va a tocar una fraccion -- un bufer de recepcion, una arena llenada a medias
 * -- vale casi cuatro veces.  Esa es una eleccion de quien lo sabe, no un
 * umbral para todos.
 *
 * \~english
 * TWO WAYS TO HAND BACK A ZEROED BLOCK, and they scale differently.  One pass
 * of `memset` costs in proportion to the SIZE and is paid whether the caller
 * looks at the memory or not.  Handing the pages back and taking them again
 * costs a system call plus a fault for each page the caller ACTUALLY touches --
 * so it follows the use instead of the size.
 *
 * PER SYSTEM, and not by a small margin: on one it pays from four mebibytes and
 * on the other it never does.  Measured with the very call the allocator makes
 * -- the fill told what the alignment is -- against giving the pages back and
 * taking them again, by how much of the block the caller then touches:
 *
 *     Linux                 1/64         1/8         all
 *       1 MiB         system 2.66x   ours 1.14x   ours 7.81x
 *       4 MiB         system 8.11x system 1.75x   ours 3.98x   <- from here
 *      16 MiB        system 20.01x system 3.46x   ours 2.29x
 *
 *     Windows               1/64         1/8         all
 *       4 MiB         system 1.97x   ours 1.69x  ours 10.95x
 *       8 MiB         system 2.05x   ours 1.55x  ours 12.85x
 *      16 MiB         system 4.99x system 1.07x   ours 6.60x   <- still loses
 *
 * AND THE RISK IS NOT SYMMETRIC, which is what decides the answer.  The caller
 * that fills the block pays the whole difference, and this side of the
 * allocator has no way of knowing which caller it has -- so the bar is that the
 * best case must be worth MORE than the worst case costs.
 *
 * OFF BY DEFAULT ON BOTH, AND THE TABLE ABOVE IS WHY IT LOOKS LIKE IT SHOULD
 * NOT BE.  Those numbers come from a benchmark of the mechanism on its own, and
 * that benchmark was wrong in the one place that decides: its "caller" touched
 * one byte per page, while a caller that reads the block reads EVERY byte, out
 * of pages that have just been faulted in.  Measured on the real thing at 8
 * MiB, the ordering flips over:
 *
 *     8 MiB, Linux        the system zeroes    we zero
 *       caller reads 1/64            27,786    105,480    system 3.9x
 *       caller reads all         1,699,725     232,295    ours   7.3x
 *
 * So the same size is nearly four times better or seven times worse depending
 * on nothing but what the caller then does -- which is exactly what a threshold
 * on SIZE cannot know.  Turning it on would be betting the fill case to win the
 * sparse one, and the fill case loses by more.
 *
 * The mechanism stays, and so does this switch, because for a caller that KNOWS
 * it will touch a fraction -- a receive buffer, an arena filled part way -- it
 * is worth nearly four times.  That is a choice for whoever knows, not a
 * threshold for everybody.
 *
 * \~
 *
 * \~english
 * @code
 *   // A program whose big zeroed blocks are mostly untouched:
 *   //   -DVESTA_ALLOC_LAZY_ZERO_MIN=(size_t(4) << 20)
 * @endcode
 *
 * \~spanish
 * @code
 *   // Un programa cuyos bloques grandes a cero casi no se tocan:
 *   //   -DVESTA_ALLOC_LAZY_ZERO_MIN=(size_t(4) << 20)
 * @endcode
 *
 * \~
 */
#ifndef VESTA_ALLOC_LAZY_ZERO_MIN
/* Bigger than any span, so it never triggers.  Written as a size and not as an
 * `#if` so the code below has ONE shape everywhere. */
#define VESTA_ALLOC_LAZY_ZERO_MIN (~size_t(0))
#endif
inline constexpr size_t kLazyZeroMin = VESTA_ALLOC_LAZY_ZERO_MIN;

/**
 * @brief
 * \~english How many bytes of freed spans a thread may hold back, at most.
 * \~spanish Cuantos bytes de tramos liberados puede guardarse un hilo, como
 *          mucho.
 * \~
 *
 * \~english
 * THIS, AND NOT THE SLOT COUNT, IS THE BOUND THAT MEANS SOMETHING.  A retained
 * span is memory nobody else can see, so it has to be capped -- but capping it
 * by "how many sizes" makes the real limit depend on which sizes the consumer
 * happens to use: two slots is 192 KiB for one program and nothing at all for
 * another.  Counting bytes gives the same promise to everybody: *this thread
 * will not sit on more than this much*.
 *
 * It also gets the trade right by itself.  Small spans are cheap to keep, so
 * many fit; a multi-megabyte one fills the budget on its own and the next free
 * goes back to the shared pool, which is exactly where something that large
 * belongs -- somebody else can use it.
 *
 * \~spanish
 * ESTO, Y NO EL NUMERO DE RANURAS, ES LA COTA QUE SIGNIFICA ALGO.  Un tramo
 * retenido es memoria que nadie mas puede ver, asi que hay que acotarlo -- pero
 * acotarlo por "cuantos tamanos" hace que el limite real dependa de que tamanos
 * use el consumidor: dos ranuras son 192 KiB para un programa y nada en
 * absoluto para otro.  Contar bytes da la misma promesa a todos: *este hilo no
 * se va a sentar encima de mas que esto*.
 *
 * Y ademas acierta el compromiso por si solo.  Los tramos pequenos son baratos
 * de guardar, asi que caben muchos; uno de varios megabytes llena el
 * presupuesto el solo y la liberacion siguiente vuelve al reparto comun, que es
 * exactamente donde debe estar algo tan grande -- otro puede usarlo.
 *
 * \~
 *
 * \~english
 * @code
 *   // Bigger for a program with few threads and large buffers:
 *   //   -DVESTA_ALLOC_SPAN_CACHE_BYTES=(16u << 20)
 * @endcode
 *
 * \~spanish
 * @code
 *   // Mayor para un programa con pocos hilos y buferes grandes:
 *   //   -DVESTA_ALLOC_SPAN_CACHE_BYTES=(16u << 20)
 * @endcode
 *
 * \~
 */
#ifndef VESTA_ALLOC_SPAN_CACHE_BYTES
#define VESTA_ALLOC_SPAN_CACHE_BYTES (size_t(2) << 20)
#endif
inline constexpr size_t kSpanCacheBytes = VESTA_ALLOC_SPAN_CACHE_BYTES;

/**
 * @brief
 * \~english The header at the start of every chunk.
 * \~spanish Cabecera al principio de cada trozo.
 * \~
 *
 * \~english
 * It takes up a whole alignment so that the blocks behind it stay 16-aligned.
 * From here comes, by merely masking the pointer, EVERYTHING needed to free:
 * what size it is and whose it is.
 *
 * IT IS NOT MOVED TO MAKE OVER-ALIGNMENT CHEAPER, and it is worth knowing why,
 * because the idea occurs to anybody who gets this far.  Since blocks start at
 * `chunk + 16` and the chunk is aligned to 64 KiB, every block address is
 * `= 16 (mod align)`: none of them is aligned to 32 or more, and that is why a
 * type with `alignas(64)` has to be served by asking for extra and pushing the
 * pointer up (see @c util::host_alloc_aligned).  Removing that 16 -- starting
 * the blocks at `chunk + class`, or moving the header out to a side table --
 * would fix it... by making ALL the chunks of ALL the classes pay: the first
 * form loses `class - 16` bytes per chunk, 25% in the large classes, to make
 * things cheaper for the few over-aligned types.
 *
 * The rule, which covers more than this: a need of ONE class of allocations is
 * met with a specialisation the caller chooses, not by changing the structure
 * all of them use.  The question that catches it is *who pays for this*.
 *
 * \~spanish
 * Ocupa una alineacion completa para que los bloques que van detras sigan
 * alineados a 16.  De aqui sale, con solo enmascarar el puntero, TODO lo que
 * hace falta para liberar: de que tamano es y de quien es.
 *
 * NO SE MUEVE PARA ABARATAR LA SOBRE-ALINEACION, y conviene saber por que
 * porque la idea se le ocurre a cualquiera que llegue hasta aqui.  Como los
 * bloques empiezan en `trozo + 16` y el trozo esta alineado a 64 KiB, toda
 * direccion de bloque es `= 16 (mod align)`: ninguna esta alineada a 32 o mas,
 * y por eso un tipo con `alignas(64)` tiene que servirse pidiendo de mas y
 * subiendo el puntero (ver @c util::host_alloc_aligned).  Quitar ese 16 --
 * empezando los bloques en `trozo + clase`, o sacando la cabecera a una tabla
 * lateral -- lo arreglaria... haciendo que TODOS los trozos de TODAS las clases
 * paguen: la primera forma pierde `clase - 16` bytes por trozo, un 25% en las
 * clases grandes, para abaratar a los pocos tipos sobre-alineados.
 *
 * La regla, que vale para mas cosas que esta: una necesidad de UNA clase de
 * reservas se resuelve con una especializacion que el llamante elige, no
 * cambiando la estructura que usan todas.  La pregunta que lo detecta es *quien
 * paga esto*.
 *
 * \~
 */
struct alignas(kAlign) ChunkHeader {
    /// \~english Which of the two things this chunk is: classes or a span.
    /// \~spanish Cual de las dos cosas es este trozo: clases o un tramo.
    /// \~
    uint32_t magic;
    /// \~english The size class, or the number of chunks if it is a span.
    /// \~spanish La clase de tamano, o el numero de trozos si es un tramo.
    /// \~
    uint32_t cls;
    /// \~english Which cache owns it; that is what says whether a free is
    ///           remote.
    /// \~spanish De que cache es; es lo que dice si una liberacion es remota.
    /// \~
    uint32_t owner;
    /**
     * @brief
     * \~english A field with MEANING, which depends on the magic.  It is not
     *          padding.
     * \~spanish Un campo con SENTIDO, que depende de la marca.  No es relleno.
     * \~
     *
     * \~english
     *   - `kSpanMagic`  -- it is `kSpanFree` when the span is free.
     *   - `kChunkMagic` in a small chunk -- zero, unused.
     *   - `kChunkMagic` in a BIG chunk   -- how many blocks have been handed
     *     out, which is where the next batch carries on from; see `grow_big`.
     *
     * It was called `_pad` from when it really was.  The three uses do not
     * clash because the magic separates them, but the old name said there was
     * nothing there and that was no longer true: a field that is read and
     * written called padding is an invitation for somebody to use it for
     * something else.
     *
     * \~spanish
     *   - `kSpanMagic`  -- vale `kSpanFree` si el tramo esta libre.
     *   - `kChunkMagic` en un trozo pequeno -- cero, no se usa.
     *   - `kChunkMagic` en un trozo GRANDE  -- cuantos bloques van entregados,
     *     que es por donde sigue la proxima tanda; ver `grow_big`.
     *
     * Se llamaba `_pad` de cuando de verdad lo era.  Los tres usos no se pisan
     * porque la marca los separa, pero el nombre viejo decia que ahi no habia
     * nada y ya no era cierto: un campo que se lee y se escribe llamado relleno
     * es una invitacion a que alguien lo use para otra cosa.
     *
     * \~
     */
    uint32_t extra;
};
static_assert(sizeof(ChunkHeader) == kAlign,
              "la cabecera descuadra los bloques");

/**
 * @brief
 * \~english How many chunks are needed for @p n bytes of user data.
 * \~spanish Cuantos trozos hacen falta para @p n bytes de usuario.
 * \~
 *
 * \~english
 * The header goes INSIDE the first chunk, so it counts towards the total.
 *
 * @par Threads
 * Safe.  It is pure arithmetic: it touches no state.
 *
 * \~spanish
 * La cabecera va DENTRO del primer trozo, asi que cuenta para el total.
 *
 * @par Hilos
 * Segura.  Es aritmetica pura: no toca ningun estado.
 *
 * \~
 * @param n
 * \~english the useful bytes wanted.
 * \~spanish los bytes utiles que se quieren.
 * \~
 * @return
 * \~english how many whole chunks that takes, header included.
 * \~spanish cuantos trozos enteros ocupa eso, cabecera incluida.
 * \~
 */
[[gnu::always_inline]] inline uint32_t chunks_for(size_t n) noexcept {
    return uint32_t((n + sizeof(ChunkHeader) + kChunkBytes - 1) / kChunkBytes);
}

namespace detail {

/**
 * @brief
 * \~english The region bounds, set once when it is put together.
 * \~spanish Limites de la region, fijados una sola vez al montarla.
 * \~
 *
 * \~english
 * They are declared here, and not in the `.cpp`, because @c in_region is on the
 * path of EVERY free and has to be able to be inline.  They are written once
 * and read billions of times, so `relaxed` is enough: they order nothing else.
 *
 * \~spanish
 * Se declaran aqui, y no en el `.cpp`, porque @c in_region esta en el camino de
 * CADA liberacion y tiene que poder estar en linea.  Se escriben una vez y se
 * leen miles de millones, asi que `relaxed` sobra: no ordenan nada mas.
 *
 * \~
 */
extern std::atomic<uintptr_t> g_region_base;
/// \~english The end of the region, one past the last byte.
/// \~spanish El final de la region, uno mas alla del ultimo byte.
/// \~
extern std::atomic<uintptr_t> g_region_end;

/// \~english The bounds of the region of the BIG classes.  See
///           @c kBigChunkBytes.  They start at zero and stay at zero while
///           nobody asks for a large allocation: the region is booked the first
///           time it is needed, not at startup.
/// \~spanish Los limites de la region de las clases GRANDES.  Ver
///           @c kBigChunkBytes.  Empiezan a cero y siguen a cero mientras nadie
///           pida una reserva grande: la region se apalabra la primera vez que
///           hace falta, no al arrancar.
/// \~
extern std::atomic<uintptr_t> g_big_base;
/// \~english The end of the big region, one past the last byte.
/// \~spanish El final de la region grande, uno mas alla del ultimo byte.
/// \~
extern std::atomic<uintptr_t> g_big_end;

/**
 * @brief
 * \~english Table from size asked for to class, in steps of 16 bytes.
 * \~spanish Tabla de tamano pedido -> clase, en pasos de 16 bytes.
 * \~
 *
 * \~english
 * One access and no branches.  It is built when the allocator is turned on; a
 * thread having a cache implies it is already built (see
 * @c host_allocator.h).
 *
 * \~spanish
 * Un acceso y sin ramas.  Se construye al activar el asignador; que un hilo
 * tenga cache implica que ya esta construida (ver @c host_allocator.h).
 *
 * \~
 */
extern uint8_t g_class_of[(kMaxSmall / kAlign) + 1];

} // namespace detail

/* Las cuatro van MARCADAS y no solo declaradas `inline`.  `inline` es una
 * sugerencia, y ya se comprobo que el compilador la descarta cuando no toca:
 * el `asm volatile` de la ranura por hilo infla su estimacion de tamano y en
 * `-O2` dejaba el asignador entero fuera de linea.  Aqui no hay nada que
 * estimar -- son de dos a cuatro instrucciones cada una -- y estan en el camino
 * de cada reserva y de cada liberacion del proceso. */

/**
 * @brief
 * \~english The class that serves @p n bytes.  Only valid for
 *          `n <= kMaxSmall`.
 * \~spanish La clase que sirve @p n bytes.  Solo vale para `n <= kMaxSmall`.
 * \~
 *
 * \~english
 * @par Threads
 * Safe.  The table is filled in ONCE, before anybody has a cache, and is read
 * only from then on; see the invariant in `host_allocator.h`.
 *
 * \~spanish
 * @par Hilos
 * Segura.  La tabla se llena UNA vez, antes de que nadie tenga cache, y a
 * partir de ahi es de solo lectura; ver el invariante de `host_allocator.h`.
 *
 * \~
 * @param n
 * \~english the size asked for, which the caller guarantees is within range.
 * \~spanish el tamano pedido, que quien llama garantiza dentro de rango.
 * \~
 * @return
 * \~english the class index, in [0, @c kClasses).
 * \~spanish el indice de clase, en [0, @c kClasses).
 * \~
 */
[[gnu::always_inline]] inline uint32_t class_of(size_t n) noexcept {
    return detail::g_class_of[(n + kAlign - 1) / kAlign];
}

/**
 * @brief
 * \~english Whether @p p came out of our region.  Two compares, no tables.
 * \~spanish Si @p p salio de nuestra region.  Dos comparaciones, sin tablas.
 * \~
 *
 * \~english
 * @par Threads
 * Safe.  The bounds are written once when the region is put together and only
 * read afterwards, so `relaxed` ordering is enough: they order nothing else.
 *
 * \~spanish
 * @par Hilos
 * Segura.  Los limites se escriben una vez al montar la region y solo se leen
 * despues, asi que el orden `relaxed` basta: no ordenan nada mas.
 *
 * \~
 * @param p
 * \~english the pointer to place.
 * \~spanish el puntero que hay que situar.
 * \~
 * @return
 * \~english true when it falls inside the region of the small classes.
 * \~spanish true cuando cae dentro de la region de las clases pequenas.
 * \~
 */
[[gnu::always_inline]] inline bool in_region(const void *p) noexcept {
    const uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return v >= detail::g_region_base.load(std::memory_order_relaxed) &&
           v < detail::g_region_end.load(std::memory_order_relaxed);
}

/**
 * @brief
 * \~english The header of the chunk @p p belongs to.  Only if @c in_region.
 * \~spanish La cabecera del trozo al que pertenece @p p.  Solo si @c in_region.
 * \~
 *
 * \~english
 * @par Threads
 * Safe.  It is a mask over the pointer; it reads no shared state.  What IS in
 * that header is shared state, and whoever reads it has to look at the magic
 * before believing it.
 *
 * \~spanish
 * @par Hilos
 * Segura.  Es una mascara sobre el puntero; no lee ningun estado compartido.
 * Lo que HAY en esa cabecera si es estado compartido, y quien lo lea tiene que
 * mirar la marca antes de creerselo.
 *
 * \~
 * @param p
 * \~english a pointer inside the region.
 * \~spanish un puntero de dentro de la region.
 * \~
 * @return
 * \~english the header of its chunk.
 * \~spanish la cabecera de su trozo.
 * \~
 */
[[gnu::always_inline]] inline ChunkHeader *chunk_of(void *p) noexcept {
    return reinterpret_cast<ChunkHeader *>(reinterpret_cast<uintptr_t>(p) &
                                           ~(uintptr_t)(kChunkBytes - 1));
}

/**
 * @brief
 * \~english Whether @p p comes out of the region of the BIG classes.
 * \~spanish Si @p p sale de la region de las clases GRANDES.
 * \~
 *
 * \~english
 * It is asked ONLY once @c in_region has already said no, so a small block
 * never runs this: its free path is the same as before this region existed.
 * With the region not set up, base and end are zero and the answer is no, with
 * no special case.
 *
 * \~spanish
 * Se pregunta SOLO cuando @c in_region ya ha dicho que no, asi que un bloque
 * pequeno nunca ejecuta esto: su camino de liberacion es el mismo que antes de
 * que esta region existiera.  Con la region sin montar, base y fin valen cero y
 * la respuesta es que no, sin caso especial.
 *
 * \~
 * @param p
 * \~english the pointer to place.
 * \~spanish el puntero que hay que situar.
 * \~
 * @return
 * \~english true when it falls inside the big region.
 * \~spanish true cuando cae dentro de la region grande.
 * \~
 */
[[gnu::always_inline]] inline bool in_big_region(const void *p) noexcept {
    const uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return v >= detail::g_big_base.load(std::memory_order_relaxed) &&
           v < detail::g_big_end.load(std::memory_order_relaxed);
}

/**
 * @brief
 * \~english The header of the big chunk @p p belongs to.
 * \~spanish La cabecera del trozo grande al que pertenece @p p.
 * \~
 *
 * \~english
 * The SAME operation as @c chunk_of with another constant, which is exactly
 * what it takes for the two kinds of chunk to live together without either
 * paying for the other.  Only valid if @c in_big_region.
 *
 * \~spanish
 * La MISMA operacion que @c chunk_of con otra constante, que es justo lo que
 * hace falta para que las dos clases de trozo convivan sin que ninguna pague
 * por la otra.  Solo vale si @c in_big_region.
 *
 * \~
 * @param p
 * \~english a pointer inside the big region.
 * \~spanish un puntero de dentro de la region grande.
 * \~
 * @return
 * \~english the header of its chunk.
 * \~spanish la cabecera de su trozo.
 * \~
 */
[[gnu::always_inline]] inline ChunkHeader *big_chunk_of(void *p) noexcept {
    return reinterpret_cast<ChunkHeader *>(reinterpret_cast<uintptr_t>(p) &
                                           ~(uintptr_t)(kBigChunkBytes - 1));
}

/**
 * @brief
 * \~english The chunk index of @p p in the region, to index in parallel.
 * \~spanish El indice de trozo de @p p en la region, para indexar en paralelo.
 * \~
 *
 * \~english
 * @par Threads
 * Safe, for the same reason as @c in_region.
 *
 * \~spanish
 * @par Hilos
 * Segura, por lo mismo que @c in_region.
 *
 * \~
 * @param p
 * \~english a pointer inside the region.
 * \~spanish un puntero de dentro de la region.
 * \~
 * @return
 * \~english its chunk index, in [0, @c kMaxChunks).
 * \~spanish su indice de trozo, en [0, @c kMaxChunks).
 * \~
 */
[[gnu::always_inline]] inline size_t chunk_index(const void *p) noexcept {
    return (reinterpret_cast<uintptr_t>(p) -
            detail::g_region_base.load(std::memory_order_relaxed)) /
           kChunkBytes;
}

} // namespace util

#endif // VESTA_UTIL_HOST_ALLOCATOR_LAYOUT_H

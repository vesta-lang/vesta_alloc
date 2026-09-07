/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/host_allocator_layout.h
 * @brief Como esta puesta la memoria del asignador: region, trozos y clases.
 *
 * POR QUE ESTA SEPARADO DEL ASIGNADOR.  Esto no es el asignador, es su
 * GEOMETRIA, y hay mas de un interesado en ella:
 *
 *   - el asignador propiamente dicho (`util/host_allocator.h`);
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
 */
#ifndef VESTA_UTIL_HOST_ALLOCATOR_LAYOUT_H
#define VESTA_UTIL_HOST_ALLOCATOR_LAYOUT_H

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace util {

/**
 * @brief Mayor reserva que sirve el asignador; por encima va al sistema.
 *
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
 * previsto -- es `AllocShape` de `util/alloc_tag.h` --, asi que es trabajo de
 * encaminamiento y no de geometria.  Hasta entonces, 16 KiB.
 */
inline constexpr size_t kMaxSmall = 16384;

/// Alineacion que garantiza `operator new` para cualquier tipo.
inline constexpr size_t kAlign = 16;

/**
 * @brief Clases de tamano.
 *
 * Paso de 16 bytes abajo y mas grueso arriba: lo que mas se pide son objetos
 * pequenos (un par de 32 bytes, un nodo de tabla), donde desperdiciar 16 bytes
 * ya es mucho, mientras que arriba las reservas son raras y el desperdicio
 * relativo es menor.  El mayor desperdicio interno queda en el 14%.
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
inline constexpr uint32_t kClasses = sizeof(kSizes) / sizeof(kSizes[0]);

/// Trozo que se pide a la region cada vez que una clase se queda sin bloques.
inline constexpr size_t kChunkBytes = 64 * 1024;

/**
 * @brief Region virtual que se reserva de una vez.
 *
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
 */
inline constexpr size_t kRegionBytes =
    sizeof(void *) >= 8 ? (size_t(256) << 30)  // 256 GiB
                        : (size_t(256) << 20); // 256 MiB

/**
 * @brief Por debajo de esto no merece la pena montar la region.
 *
 * @c kRegionBytes es lo que se PIDE, no lo que se consigue: se pregunta al
 * sistema bajando a la mitad hasta que entre (`util::os_reserve_largest`), asi
 * que en una maquina apretada se acaba con menos y se sigue funcionando.  Este
 * es el suelo: con 64 MiB caben mil trozos, que ya da para que el asignador
 * aporte algo; por debajo, mejor ser sincero y no montarlo.
 */
inline constexpr size_t kRegionMinBytes = size_t(64) << 20; // 64 MiB

/// Cuantos trozos caben en la region.  Lo necesita quien indexe POR trozo.
inline constexpr size_t kMaxChunks = kRegionBytes / kChunkBytes;

/**
 * @brief El trozo de las clases GRANDES, que no es el mismo que el de las
 *        pequenas y por eso vive en su propia region.
 *
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
 */
inline constexpr size_t kBigChunkBytes = size_t(1) << 20; // 1 MiB

/**
 * @brief Desde que tamano de clase se sirve de la region grande.
 *
 * Por debajo de 2 KiB el desperdicio de un trozo de 64 KiB se queda en el 3% y
 * casi siempre por debajo del 1,5%, asi que mudarlas no compraria nada y en
 * cambio comprometeria 1 MiB por clase y por hilo.  Desde 2 KiB el desperdicio
 * empieza a subir y no para.
 */
inline constexpr size_t kBigClassMin = 2048;

/// Direcciones que se apalabran para las clases grandes.  Mucho menos que la
/// region pequena porque estas reservas son raras -- el 0,9% --, y aun asi
/// sobrado: son 16.384 trozos de 1 MiB.
inline constexpr size_t kBigRegionBytes = size_t(16) << 30;   // 16 GiB
inline constexpr size_t kBigRegionMinBytes = size_t(32) << 20; // 32 MiB

/**
 * @brief Tope de identificadores de cache: cuantos duenos distintos puede
 *        nombrar la cabecera de un trozo.
 *
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
 */
inline constexpr uint32_t kMaxThreads = 64;

/**
 * @brief Owner ids reserved for the LOCK-FREE per-thread policy.
 *
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
 */
inline constexpr uint32_t kPerThreadCaches = 960;

/// Entries in the cache and remote tables: both policies index the same arrays.
inline constexpr uint32_t kTotalCaches = kMaxThreads + kPerThreadCaches;

static_assert(kPerThreadCaches % 64 == 0,
              "the free-id map is an array of 64-bit words, so the count has "
              "to fill whole words: a partial word would hand out ids that do "
              "not exist");

/// Marca de un trozo troceado en clases, el del asignador normal.
inline constexpr uint32_t kChunkMagic = 0x56455354u; // 'VEST'

/**
 * @brief Marca de un TRAMO: varios trozos seguidos que son UNA sola reserva.
 *
 * Es como se sirven las reservas grandes sin pedirselas a nadie.  El trozo
 * sigue siendo la unidad de direccionamiento -- la cabecera de un puntero se
 * localiza enmascarando, una instruccion --, y un tramo no es mas que N trozos
 * seguidos cuya cabecera esta en el primero.  Como al usuario se le entrega
 * siempre `cabeza + sizeof(ChunkHeader)`, `chunk_of()` cae en la cabeza sola y
 * el camino de liberar no cambia.
 */
inline constexpr uint32_t kSpanMagic = 0x5350414eu; // 'SPAN'

/**
 * @brief Hasta que tamano se GUARDA un tramo para reusarlo, en trozos.
 *
 * **No es un tope de lo que se puede pedir.**  Una reserva mayor se sirve
 * igual; lo unico que cambia es que al soltarla se devuelve al sistema en vez
 * de quedarsela, porque guardar un bloque de mas de 16 MiB por si vuelve a
 * hacer falta cuesta mas de lo que ahorra.
 *
 * 256 trozos son 16 MiB, que es donde acaba la cola medida HOY (ver
 * `doc/PLAN_RESERVAS.md`): 17 reservas de toda una compilacion caen entre 1 y
 * 16 MiB y ninguna por encima.  Manana puede ser otra cifra; por eso es una
 * cota de cache y no una condicion de funcionamiento.
 */
inline constexpr uint32_t kMaxSpanChunks = 256;

/**
 * @brief Cuantos tamanos de tramo se guarda cada hilo para si, sin cerrojo.
 *
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
 * POR QUE SOLO DOS.  Porque un tramo guardado esta RETENIDO: no lo ve ninguna
 * otra parte y, si el hilo muere, se pierde.  Con dos tamanos (uno y dos
 * trozos, o sea hasta 128 KiB) el techo es de 192 KiB por hilo, y ahi caen el
 * 90% de las reservas grandes segun el reparto medido.  Subirlo cambiaria unos
 * nanosegundos raros por memoria retenida en todos los hilos.
 */
inline constexpr uint32_t kSpanCacheSlots = 2;

/**
 * @brief Cabecera al principio de cada trozo.
 *
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
 * paga esto*.  Escrito con sus numeros en D18 de `doc/PLAN_RESERVAS.md`.
 */
struct alignas(kAlign) ChunkHeader {
    uint32_t magic;
    uint32_t cls;
    uint32_t owner;
    /**
     * @brief Un campo con SENTIDO, que depende de la marca.  No es relleno.
     *
     *   - `kSpanMagic`  -- vale `kSpanFree` si el tramo esta libre.
     *   - `kChunkMagic` en un trozo pequeno -- cero, no se usa.
     *   - `kChunkMagic` en un trozo GRANDE  -- cuantos bloques van entregados,
     *     que es por donde sigue la proxima tanda; ver `grow_big`.
     *
     * Se llamaba `_pad` de cuando de verdad lo era.  Los tres usos no se pisan
     * porque la marca los separa, pero el nombre viejo decia que ahi no habia
     * nada y ya no era cierto: un campo que se lee y se escribe llamado relleno
     * es una invitacion a que alguien lo use para otra cosa.
     */
    uint32_t extra;
};
static_assert(sizeof(ChunkHeader) == kAlign,
              "la cabecera descuadra los bloques");

/**
 * @brief Cuantos trozos hacen falta para @p n bytes de usuario.
 *
 * La cabecera va DENTRO del primer trozo, asi que cuenta para el total.
 *
 * @par Hilos
 * Segura.  Es aritmetica pura: no toca ningun estado.
 */
[[gnu::always_inline]] inline uint32_t chunks_for(size_t n) noexcept {
    return uint32_t((n + sizeof(ChunkHeader) + kChunkBytes - 1) / kChunkBytes);
}

namespace detail {

/**
 * @brief Limites de la region, fijados una sola vez al montarla.
 *
 * Se declaran aqui, y no en el `.cpp`, porque @c in_region esta en el camino de
 * CADA liberacion y tiene que poder estar en linea.  Se escriben una vez y se
 * leen miles de millones, asi que `relaxed` sobra: no ordenan nada mas.
 */
extern std::atomic<uintptr_t> g_region_base;
extern std::atomic<uintptr_t> g_region_end;

/// Los limites de la region de las clases GRANDES.  Ver @c kBigChunkBytes.
/// Empiezan a cero y siguen a cero mientras nadie pida una reserva grande: la
/// region se apalabra la primera vez que hace falta, no al arrancar.
extern std::atomic<uintptr_t> g_big_base;
extern std::atomic<uintptr_t> g_big_end;

/**
 * @brief Tabla de tamano pedido -> clase, en pasos de 16 bytes.
 *
 * Un acceso y sin ramas.  Se construye al activar el asignador; que un hilo
 * tenga cache implica que ya esta construida (ver @c host_allocator.h).
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
 * @brief La clase que sirve @p n bytes.  Solo vale para `n <= kMaxSmall`.
 *
 * @par Hilos
 * Segura.  La tabla se llena UNA vez, antes de que nadie tenga cache, y a
 * partir de ahi es de solo lectura; ver el invariante de `host_allocator.h`.
 */
[[gnu::always_inline]] inline uint32_t class_of(size_t n) noexcept {
    return detail::g_class_of[(n + kAlign - 1) / kAlign];
}

/**
 * @brief Si @p p salio de nuestra region.  Dos comparaciones, sin tablas.
 *
 * @par Hilos
 * Segura.  Los limites se escriben una vez al montar la region y solo se leen
 * despues, asi que el orden `relaxed` basta: no ordenan nada mas.
 */
[[gnu::always_inline]] inline bool in_region(const void *p) noexcept {
    const uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return v >= detail::g_region_base.load(std::memory_order_relaxed) &&
           v < detail::g_region_end.load(std::memory_order_relaxed);
}

/**
 * @brief La cabecera del trozo al que pertenece @p p.  Solo si @c in_region.
 *
 * @par Hilos
 * Segura.  Es una mascara sobre el puntero; no lee ningun estado compartido.
 * Lo que HAY en esa cabecera si es estado compartido, y quien lo lea tiene que
 * mirar la marca antes de creerselo.
 */
[[gnu::always_inline]] inline ChunkHeader *chunk_of(void *p) noexcept {
    return reinterpret_cast<ChunkHeader *>(reinterpret_cast<uintptr_t>(p) &
                                           ~(uintptr_t)(kChunkBytes - 1));
}

/**
 * @brief Si @p p sale de la region de las clases GRANDES.
 *
 * Se pregunta SOLO cuando @c in_region ya ha dicho que no, asi que un bloque
 * pequeno nunca ejecuta esto: su camino de liberacion es el mismo que antes de
 * que esta region existiera.  Con la region sin montar, base y fin valen cero y
 * la respuesta es que no, sin caso especial.
 */
[[gnu::always_inline]] inline bool in_big_region(const void *p) noexcept {
    const uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return v >= detail::g_big_base.load(std::memory_order_relaxed) &&
           v < detail::g_big_end.load(std::memory_order_relaxed);
}

/**
 * @brief La cabecera del trozo grande al que pertenece @p p.
 *
 * La MISMA operacion que @c chunk_of con otra constante, que es justo lo que
 * hace falta para que las dos clases de trozo convivan sin que ninguna pague
 * por la otra.  Solo vale si @c in_big_region.
 */
[[gnu::always_inline]] inline ChunkHeader *big_chunk_of(void *p) noexcept {
    return reinterpret_cast<ChunkHeader *>(reinterpret_cast<uintptr_t>(p) &
                                           ~(uintptr_t)(kBigChunkBytes - 1));
}

/**
 * @brief El indice de trozo de @p p en la region, para indexar en paralelo.
 *
 * @par Hilos
 * Segura, por lo mismo que @c in_region.
 */
[[gnu::always_inline]] inline size_t chunk_index(const void *p) noexcept {
    return (reinterpret_cast<uintptr_t>(p) -
            detail::g_region_base.load(std::memory_order_relaxed)) /
           kChunkBytes;
}

} // namespace util

#endif // VESTA_UTIL_HOST_ALLOCATOR_LAYOUT_H

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
 */
inline constexpr size_t kMaxSmall = 2048;

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
    1152, 1280, 1408, 1536, 1792, 2048};
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

/// Tope de hilos con listas propias.  Al pasarse, ese hilo va al sistema.
inline constexpr uint32_t kMaxThreads = 64;

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
 * @brief Cabecera al principio de cada trozo.
 *
 * Ocupa una alineacion completa para que los bloques que van detras sigan
 * alineados a 16.  De aqui sale, con solo enmascarar el puntero, TODO lo que
 * hace falta para liberar: de que tamano es y de quien es.
 */
struct alignas(kAlign) ChunkHeader {
    uint32_t magic;
    uint32_t cls;
    uint32_t owner;
    uint32_t _pad;
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

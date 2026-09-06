/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/os_memory.h
 * @brief Pedir memoria AL SISTEMA, separando apalabrar de entregar.
 *
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
 */
#ifndef VESTA_UTIL_OS_MEMORY_H
#define VESTA_UTIL_OS_MEMORY_H

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace util {

namespace detail {

/**
 * @brief Lo que dice el sistema de sus tamanos, preguntado UNA sola vez.
 *
 * Declarados aqui, y no escondidos en el `.cpp`, para que consultarlos sea una
 * lectura y no una llamada.  `vm::allocate_memory` redondea a paginas en CADA
 * reserva: antes hacia un `GetSystemInfo` (cuarenta bytes de estructura en la
 * pila mas una consulta al sistema) para averiguar un numero que no cambia
 * nunca, y eso solo bastaba para que la funcion no cupiera dentro de quien la
 * llamaba.
 *
 * Cero significa "todavia no se ha preguntado".  No son API: se leen por
 * @c os_page_size y @c os_reserve_granularity.
 */
extern std::atomic<size_t> g_page_size;
extern std::atomic<size_t> g_granularity;

/// Pregunta al sistema y rellena los dos.  Fuera de linea: pasa una vez.
void query_os_sizes() noexcept;

} // namespace detail

/**
 * @brief Permisos de una region de memoria.
 *
 * Bits sueltos para poder combinarlos con `|`.  Se traducen a lo que entienda
 * el sistema DENTRO del `.cpp`; quien llama no tiene que saber si esto acaba en
 * un `PAGE_EXECUTE_READWRITE` o en un `PROT_READ | PROT_EXEC`, que es
 * justamente lo que permite que esta cabecera no incluya nada del sistema.
 */
enum class OsProt : unsigned {
    None = 0,
    Read = 1u << 0,
    Write = 1u << 1,
    Exec = 1u << 2, ///< paginas de codigo generado (JIT)
};

constexpr OsProt operator|(OsProt a, OsProt b) {
    return static_cast<OsProt>(static_cast<unsigned>(a) |
                               static_cast<unsigned>(b));
}
constexpr bool has_prot(OsProt set, OsProt bit) {
    return (static_cast<unsigned>(set) & static_cast<unsigned>(bit)) != 0;
}

/// Lo que se pide casi siempre: leer y escribir.
inline constexpr OsProt kOsReadWrite = OsProt::Read | OsProt::Write;

/**
 * @brief Apalabra y ENTREGA de una vez, con los permisos pedidos.
 * @param bytes Se redondea a paginas hacia arriba.
 * @return La base del bloque, o nullptr si el sistema no puede.
 *
 * Es lo que quiere quien solo necesita un bloque y ya: apalabrar y entregar
 * por separado solo compensa cuando se va a entregar POR PARTES.
 *
 * @par Hilos
 * Segura desde cualquier hilo.
 *
 * @code
 *   // Una pagina de codigo generado.
 *   void *code = util::os_alloc(4096, util::OsProt::Read |
 *                                     util::OsProt::Write |
 *                                     util::OsProt::Exec);
 *   if (code == nullptr) return false;
 *   ...
 *   util::os_free(code, 4096);
 * @endcode
 */
void *os_alloc(size_t bytes, OsProt prot) noexcept;

/**
 * @brief Suelta un bloque de @c os_alloc.
 * @param bytes El mismo tamano que se pidio, antes de redondear.
 *
 * @par Hilos
 * Segura.
 */
void os_free(void *addr, size_t bytes) noexcept;

/**
 * @brief Cambia los permisos de un tramo ya entregado.
 * @return false si el sistema no lo permite.
 *
 * Lo usa quien escribe codigo y luego quiere ejecutarlo: se entrega con
 * escritura, se rellena, y se pasa a ejecucion.  Tener las dos cosas a la vez
 * funciona, pero deja paginas escribibles Y ejecutables, que es lo que ningun
 * sistema operativo moderno quiere ver.
 *
 * @par Hilos
 * Segura mientras los tramos no se solapen.
 *
 * @code
 *   std::memcpy(code, bytes, n);                       // escribir
 *   util::os_protect(code, n, util::OsProt::Read |
 *                             util::OsProt::Exec);     // y ya solo ejecutar
 * @endcode
 */
bool os_protect(void *addr, size_t bytes, OsProt prot) noexcept;

/**
 * @brief Apalabra @p bytes de DIRECCIONES sin gastar memoria.
 * @return La base del rango, o nullptr si el sistema no puede.
 *
 * Lo devuelto NO se puede leer ni escribir hasta pasarlo por @c os_commit.
 * Apalabrar consume espacio de direcciones, que en 64 bits sobra, pero **no es
 * gratis del todo**: el sistema apunta la contabilidad de paginas, y sale a
 * unos 2,7 MiB y medio milisegundo por TiB apalabrado.
 *
 * @par Hilos
 * Segura desde cualquier hilo.  Cada llamada devuelve un rango distinto.
 *
 * @code
 *   // Apalabrar 1 GiB y usar solo el primer trozo.
 *   void *base = util::os_reserve(size_t(1) << 30);
 *   if (base == nullptr) return false;              // el sistema dijo que no
 *   if (!util::os_commit(base, 64 * 1024)) {        // entregar 64 KiB
 *       util::os_release(base, size_t(1) << 30);
 *       return false;
 *   }
 *   std::memset(base, 0, 64 * 1024);               // ya se puede escribir
 * @endcode
 */
void *os_reserve(size_t bytes) noexcept;

/**
 * @brief Entrega memoria de verdad en un tramo ya apalabrado, lectura/escritura.
 * @param addr  Dentro de un rango devuelto por @c os_reserve.
 * @param bytes Tamano del tramo; se redondea a paginas hacia arriba.
 * @return false si el sistema no puede dar mas memoria.
 *
 * Entregar dos veces el mismo tramo no es un error: la segunda no hace nada.
 *
 * @par Hilos
 * Segura desde cualquier hilo **siempre que los tramos no se solapen**.  Dos
 * hilos entregando el MISMO tramo a la vez es correcto en Windows y en POSIX,
 * pero quien reparta los tramos ya deberia estar impidiendolo.
 *
 * @code
 *   // Crecer un trozo cada vez, segun hace falta.
 *   char *page = static_cast<char *>(base) + usados;
 *   if (!util::os_commit(page, 64 * 1024)) return nullptr;  // sin memoria
 *   usados += 64 * 1024;
 * @endcode
 */
bool os_commit(void *addr, size_t bytes, OsProt prot = kOsReadWrite) noexcept;

/**
 * @brief Devuelve la memoria de un tramo al sistema, CONSERVANDO su direccion.
 *
 * Es lo que permite bajar el pico sin perder el rango: el tramo sigue siendo
 * nuestro y se puede volver a entregar con @c os_commit.  **Lo que hubiera
 * dentro se pierde**, y volver a leerlo sin entregarlo otra vez falla -- que es
 * lo que se quiere: un uso despues de soltar tiene que dar un fallo ruidoso, no
 * ceros en silencio.
 *
 * @par Hilos
 * Segura, con la misma condicion que @c os_commit: tramos que no se solapen.
 *
 * @code
 *   // Un tramo que se quedo vacio: se sueltan sus paginas y se guarda el sitio
 *   // para reusarlo sin volver a pedirle direcciones al sistema.
 *   util::os_decommit(tramo, bytes);
 *   guardar_para_reusar(tramo, bytes);
 *   // ...mas tarde
 *   util::os_commit(tramo, bytes);   // vuelve a tener memoria detras
 * @endcode
 */
bool os_decommit(void *addr, size_t bytes) noexcept;

/**
 * @brief Suelta un rango apalabrado ENTERO.
 * @param addr  Exactamente lo que devolvio @c os_reserve.
 * @param bytes El mismo tamano que se le pidio.
 *
 * @par Hilos
 * Segura, pero soltar un rango que otro hilo este usando es un fallo del que
 * llama, no de aqui.
 *
 * @code
 *   util::os_release(base, size_t(1) << 30);   // el rango deja de ser nuestro
 * @endcode
 */
void os_release(void *addr, size_t bytes) noexcept;

/**
 * @brief Tamano de pagina del sistema.  Se consulta una vez y se recuerda.
 *
 * @par Hilos
 * Segura.  Dos hilos que lleguen los primeros preguntan los dos y escriben lo
 * mismo, que no es un problema; no hay variable de guarda de por medio (esas ya
 * costaron un cuelgue, ver `util/thread_slot.h`).
 *
 * @code
 *   const size_t page = util::os_page_size();
 *   const size_t redondeado = (n + page - 1) & ~(page - 1);
 * @endcode
 */
[[gnu::always_inline]] inline size_t os_page_size() noexcept {
    const size_t v = detail::g_page_size.load(std::memory_order_relaxed);
    if (v != 0) return v;
    detail::query_os_sizes();
    return detail::g_page_size.load(std::memory_order_relaxed);
}

/**
 * @brief Con que alineacion entrega direcciones @c os_reserve.
 *
 * En Windows son 64 KiB, no el tamano de pagina, y hay codigo que depende de
 * ello -- el asignador localiza el trozo de un puntero enmascarando --, asi que
 * se pregunta en vez de suponerse.
 *
 * @par Hilos
 * Segura, igual que @c os_page_size.
 *
 * @code
 *   // Alinear la base a lo que de verdad entrega el sistema, no a lo supuesto.
 *   const uintptr_t g = util::os_reserve_granularity();
 *   const uintptr_t base = (bruta + g - 1) & ~(g - 1);
 * @endcode
 */
[[gnu::always_inline]] inline size_t os_reserve_granularity() noexcept {
    const size_t v = detail::g_granularity.load(std::memory_order_relaxed);
    if (v != 0) return v;
    detail::query_os_sizes();
    return detail::g_granularity.load(std::memory_order_relaxed);
}

/**
 * @brief Apalabra el mayor rango que el sistema conceda entre dos limites.
 * @param max     Lo que se querria.
 * @param min     Por debajo de esto no merece la pena; se rinde y da nullptr.
 * @param got     Sale cuanto se consiguio de verdad.  Cero si no se pudo nada.
 * @return La base del rango, o nullptr.
 *
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
 * @code
 *   // Pedir 256 GiB, conformarse con 64 MiB, y SABER con cuanto se quedo uno.
 *   size_t conseguido = 0;
 *   void *base = util::os_reserve_largest(size_t(256) << 30,
 *                                         size_t(64) << 20, &conseguido);
 *   if (base == nullptr) return false;      // ni el minimo: no hay region
 *   // `conseguido` es lo que hay de verdad; usarlo, no el maximo que se pidio.
 * @endcode
 */
void *os_reserve_largest(size_t max, size_t min, size_t *got) noexcept;

/**
 * @brief Lo que el sistema dice de la memoria de ESTE proceso, ahora mismo.
 *
 * Es una pregunta DISTINTA de la que contesta `util::host_alloc_stats()`, y
 * conviene no confundirlas: aquella dice lo que pidio NUESTRO asignador, esta
 * lo que el sistema operativo tiene apuntado del proceso entero -- incluido lo
 * que reservaron las librerias de terceros, el codigo del propio ejecutable y
 * las pilas de los hilos.
 *
 * Todo en bytes.  Un campo a cero significa que este sistema no lo sabe decir.
 */
struct OsProcessMemory {
    uint64_t working_set = 0;      ///< residente en RAM ahora mismo
    uint64_t working_set_peak = 0; ///< lo mayor que llego a estar residente
    uint64_t commit = 0;           ///< memoria privada comprometida
    uint64_t commit_peak = 0;      ///< el mayor valor que alcanzo
};

/**
 * @brief Pregunta al sistema por este proceso.
 *
 * No reserva memoria, asi que se puede llamar desde un sitio apretado -- justo
 * cuando el sistema acaba de decir que no --.  No esta pensada para un camino
 * caliente: por dentro consulta al sistema.
 *
 * @par Hilos
 * Segura desde cualquier hilo.  Devuelve una copia, no una referencia a estado.
 *
 * @code
 *   // Cuanto llego a ocupar ESTE proceso, que es la pregunta que los
 *   // contadores del asignador NO contestan: ahi entra tambien lo de terceros.
 *   const auto m = util::os_process_memory();
 *   std::printf("pico %.1f MiB\n", m.working_set_peak / (1024.0 * 1024.0));
 * @endcode
 */
OsProcessMemory os_process_memory() noexcept;

/**
 * @brief Lo que el sistema dice de SI MISMO.
 *
 * `physical_available` y `address_space_free` CAMBIAN, asi que se preguntan
 * cada vez; los otros dos no, pero salen en el mismo viaje.
 */
struct OsSystemMemory {
    uint64_t physical_total = 0;      ///< RAM instalada
    uint64_t physical_available = 0;  ///< RAM libre ahora
    uint64_t address_space_total = 0; ///< cuanto puede direccionar el proceso
    uint64_t address_space_free = 0;  ///< cuanto queda sin apalabrar
};

/**
 * @brief Pregunta al sistema por si mismo.  Mismas propiedades que la de arriba.
 *
 * @par Hilos
 * Segura desde cualquier hilo.
 *
 * @code
 *   // Dimensionar segun lo que HAY, no segun una constante escrita a mano.
 *   const auto s = util::os_system_memory();
 *   const size_t cache = s.physical_available / 8;   // un octavo de lo libre
 * @endcode
 */
OsSystemMemory os_system_memory() noexcept;

} // namespace util

#endif // VESTA_UTIL_OS_MEMORY_H

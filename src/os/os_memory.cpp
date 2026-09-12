/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/os_memory.cpp
 * @brief Implementacion del trato con el sistema.  Los motivos, en la cabecera.
 *
 * Aqui SI se pueden incluir las cabeceras del sistema: es una unidad suelta y
 * lo que definen -- `VOID` entre otras cosas -- no sale de aqui.
 *
 * REGLA DE ESTE FICHERO: **ninguna funcion reserva memoria, ni imprime, ni
 * lanza.**  Por debajo de esto no hay asignador al que caer, asi que un fallo
 * es un valor de retorno y nada mas.
 */

#include "util/os/os_memory.h"

#include <atomic>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
/* Ya no hace falta `psapi` ni pedir su version 2: la memoria del proceso se le
 * pregunta a ntdll (`NtQueryInformationProcess`), que es a quien acababa
 * preguntando `GetProcessMemoryInfo`. */
#include <windows.h>
#else
#include <cstdio> // os_module_base lee /proc/self/maps
#include <fcntl.h>
#include <sched.h> // os_yield where there is no direct syscall
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

#if defined(_WIN32)
extern "C" {
LONG __stdcall NtAllocateVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress,
                                       ULONG_PTR ZeroBits, PSIZE_T RegionSize,
                                       ULONG AllocationType, ULONG Protect);
LONG __stdcall NtFreeVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress,
                                   PSIZE_T RegionSize, ULONG FreeType);
LONG __stdcall NtProtectVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress,
                                      PSIZE_T RegionSize, ULONG NewProtect,
                                      PULONG OldProtect);
/* Las dos consultas, para no tener que llamar a kernel32 ni para preguntar.  No
 * son camino caliente -- una pasa una vez y la otra es diagnostico --, asi que
 * aqui el motivo no es el tiempo: es que la capa de memoria deje de depender de
 * kernel32, que es la direccion en la que va el proyecto. */
LONG __stdcall NtQuerySystemInformation(ULONG SystemInformationClass,
                                        PVOID SystemInformation,
                                        ULONG SystemInformationLength,
                                        PULONG ReturnLength);
LONG __stdcall NtQueryInformationProcess(HANDLE ProcessHandle,
                                         ULONG ProcessInformationClass,
                                         PVOID ProcessInformation,
                                         ULONG ProcessInformationLength,
                                         PULONG ReturnLength);
/* Que hay en una direccion.  La usa `os_alloc_near` para recorrer el mapa
 * buscando hueco, que es lo unico que permite colocar un bloque CERCA de otro
 * en vez de dejarlo al azar del sistema. */
LONG __stdcall NtQueryVirtualMemory(HANDLE ProcessHandle, PVOID BaseAddress,
                                    ULONG MemoryInformationClass,
                                    PVOID MemoryInformation,
                                    SIZE_T MemoryInformationLength,
                                    PSIZE_T ReturnLength);
}

/// Lo que devuelve `NtQuerySystemInformation` con la clase 0.  Se escribe aqui
/// porque `<winternl.h>` la declara recortada y lo que hace falta -- el tamano
/// de pagina y la granularidad -- esta pasada la parte que declara.
struct VestaSystemBasicInformation {
    ULONG Reserved;
    ULONG TimerResolution;
    ULONG PageSize;
    ULONG NumberOfPhysicalPages;
    ULONG LowestPhysicalPageNumber;
    ULONG HighestPhysicalPageNumber;
    ULONG AllocationGranularity;
    ULONG_PTR MinimumUserModeAddress;
    ULONG_PTR MaximumUserModeAddress;
    ULONG_PTR ActiveProcessorsAffinityMask;
    CCHAR NumberOfProcessors;
};

/// Contadores de memoria del proceso, clase 3 de `NtQueryInformationProcess`.
/// Es lo mismo que `GetProcessMemoryInfo` devuelve, sin pasar por psapi.
struct VestaVmCounters {
    SIZE_T PeakVirtualSize;
    SIZE_T VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivateUsage;
};
#endif

namespace util {

namespace detail {

/* Atomicos sueltos y no un estatico de funcion: un estatico de funcion lleva
 * variable de guarda, y las guardas en este toolchain ya costaron un cuelgue
 * (ver `util/os/thread_slot.h`).  Dos hilos que lleguen a la vez preguntan los dos
 * y escriben lo mismo, que no es un problema. */
std::atomic<size_t> g_page_size{0};
std::atomic<size_t> g_granularity{0};

void query_os_sizes() noexcept {
#if defined(_WIN32)
    /* Se le pregunta a ntdll, no a `GetSystemInfo`: aquella rellena cuarenta
     * bytes de estructura de los que aqui interesan dos campos.  Y si por lo
     * que fuera fallara, se usan los valores de siempre de x86-64 en vez de
     * quedarse a cero, que dejaria a `page_range` dividiendo por nada. */
    VestaSystemBasicInformation sbi;
    if (NtQuerySystemInformation(0 /* SystemBasicInformation */, &sbi,
                                 sizeof(sbi), nullptr) >= 0) {
        g_page_size.store(sbi.PageSize, std::memory_order_relaxed);
        g_granularity.store(sbi.AllocationGranularity,
                            std::memory_order_relaxed);
    } else {
        g_page_size.store(4096, std::memory_order_relaxed);
        g_granularity.store(64 * 1024, std::memory_order_relaxed);
    }
#else
    const long ps = sysconf(_SC_PAGESIZE);
    const size_t v = ps > 0 ? (size_t)ps : 4096;
    g_page_size.store(v, std::memory_order_relaxed);
    // En POSIX `mmap` entrega alineado a pagina y no hay una granularidad
    // aparte, asi que son la misma cosa.
    g_granularity.store(v, std::memory_order_relaxed);
#endif
}

} // namespace detail

using detail::g_granularity;
using detail::g_page_size;
using detail::query_os_sizes;

namespace {

/// Redondea @p n hacia arriba al multiplo de @p a.  @p a es potencia de dos.
inline size_t round_up(size_t n, size_t a) noexcept {
    return (n + a - 1) & ~(a - 1);
}

/**
 * @brief Lleva un rango a paginas ENTERAS: la base hacia abajo y el final hacia
 *        arriba.
 *
 * POR QUE HACE FALTA, y por que es un arreglo y no una comodidad.  `mprotect` y
 * `madvise` EXIGEN que la direccion este en una pagina y fallan si no lo esta;
 * `VirtualAlloc` de Windows la redondea sola.  O sea que la misma llamada
 * funcionaba en un sistema y fallaba en el otro -- y fallaba devolviendo
 * `false`, que quien llama lee como "no hay memoria" y resuelve degradandose.
 *
 * Costo encontrarlo: en Linux las clases grandes se salian de su region y
 * volvian al camino de siempre sin que nada lo dijera, porque el respaldo
 * funciona.  Lo destapo una prueba que comprobaba de QUE region sale cada
 * reserva; el tiempo y la memoria no se movian lo bastante para notarlo.
 *
 * Normalizando aqui, @c os_commit y @c os_decommit significan lo MISMO en los
 * dos sistemas, que es justo lo que se le pide a esta capa.
 */
inline void page_range(void *&addr, size_t &bytes) noexcept {
    const size_t page = os_page_size();
    const uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    const uintptr_t base = a & ~(uintptr_t)(page - 1);
    bytes = round_up(bytes + size_t(a - base), page);
    addr = reinterpret_cast<void *>(base);
}

/// Los tres bits de permisos como indice.  El enmascarado sobra si el valor
/// viene de @c OsProt, pero cuesta cero y evita salirse de la tabla si alguien
/// convierte un entero cualquiera.
constexpr unsigned prot_index(OsProt p) noexcept {
    return static_cast<unsigned>(p) & 7u;
}

} // namespace

/* `os_page_size` y `os_reserve_granularity` estan EN LINEA en la cabecera: son
 * una lectura relajada y una rama predicha, y las llama `vm::allocate_memory`
 * en cada reserva para redondear a paginas.  Aqui solo vive lo que hay que
 * preguntarle al sistema, que pasa una vez. */

#if defined(_WIN32)

/**
 * @brief Traduce nuestros permisos a la constante del sistema.
 *
 * Los permisos son TRES BITS, asi que la traduccion es una TABLA: ocho
 * combinaciones y ni una rama.  Y siendo `constexpr`, en los sitios donde los
 * permisos son una constante escrita a mano -- que son casi todos, porque nadie
 * calcula si quiere ejecucion -- el compilador resuelve el indexado y lo que
 * queda es el numero.
 *
 * Vive AQUI y no en la cabecera a proposito: es lo unico que necesita las
 * constantes `PAGE_*`, y tenerlo dentro es lo que permite que la cabecera no
 * incluya `windows.h`.  Esa cabecera define `VOID` como macro y rompe cualquier
 * `enum class` que use ese nombre; ya obligo a aislar dos ficheros del proyecto.
 *
 * Dos casillas repetidas no son un descuido: Windows no tiene proteccion de
 * SOLO escritura ni de escritura MAS ejecucion sin lectura, asi que las dos
 * suben a la variante con lectura.  Es lo unico que se puede hacer, y esta
 * anotado para que no parezca un error de copia.
 */
constexpr DWORD kWinProt[8] = {
    PAGE_NOACCESS,          // 000  ---
    PAGE_READONLY,          // 001  R
    PAGE_READWRITE,         // 010  -W-   -> no existe solo escritura
    PAGE_READWRITE,         // 011  RW
    PAGE_EXECUTE,           // 100  --X
    PAGE_EXECUTE_READ,      // 101  R-X
    PAGE_EXECUTE_READWRITE, // 110  -WX   -> no existe sin lectura
    PAGE_EXECUTE_READWRITE, // 111  RWX
};

// -------------------------------------------------------------------------
//  La memoria se le pide a NTDLL, no a kernel32
// -------------------------------------------------------------------------
//
// SE LE PIDE A NTDLL, y ENLAZANDO, no resolviendo.
//
// `VirtualAlloc`, `VirtualFree` y `VirtualProtect` no hacen el trabajo: validan
// argumentos, traducen banderas y acaban llamando a estas tres.  Saltarse esa
// capa aqui SI cuenta, al reves que en un lector de ficheros: `os_commit` se
// llama en cada recarga de trozo, o sea a lo largo de toda una compilacion, no
// una vez por fichero.
//
// Y enlazadas, no resueltas con `GetProcAddress`.  Un intento anterior las
// guardaba en punteros atomicos, y eso mete en CADA llamada una carga y una
// comparacion con nulo que el enlazado no necesita -- el sitio de llamada queda
// en un `call` indirecto por la tabla de importacion, que es lo mismo que
// costaba llamar a `VirtualAlloc` --.  Ademas convierte un simbolo que falta
// -- que no seria una version antigua sino un sistema roto -- en un fallo que
// aparece tarde y lejos, en vez de al cargar el proceso.
//
// El prototipo se escribe aqui y no se saca de `<winternl.h>`: esa cabecera
// declara la mitad de estas y arrastra tipos que no hacen falta.

/// El proceso actual.  Es una pseudo-asa constante; no hay que cerrarla.
#define VESTA_NT_SELF (reinterpret_cast<HANDLE>(static_cast<intptr_t>(-1)))


/// Suelta el rango ENTERO.  Con `MEM_RELEASE` el tamano tiene que ir a cero y
/// la base tiene que ser la que devolvio la reserva.
void release_range(void *addr) noexcept {
    if (addr == nullptr) return;
    PVOID base = addr;
    SIZE_T size = 0;
    NtFreeVirtualMemory(VESTA_NT_SELF, &base, &size, MEM_RELEASE);
}

void *os_reserve(size_t bytes) noexcept {
    if (bytes == 0) return nullptr;
    /* `PAGE_READWRITE` en una reserva no entrega nada: es el permiso que
     * tendran las paginas cuando se comprometan.  Mientras no se comprometan,
     * tocarlas falla igual. */
    PVOID base = nullptr;
    SIZE_T size = bytes;
    return NtAllocateVirtualMemory(VESTA_NT_SELF, &base, 0, &size, MEM_RESERVE,
                                   PAGE_READWRITE) >= 0
               ? base
               : nullptr;
}

bool os_commit(void *addr, size_t bytes, OsProt prot) noexcept {
    if (addr == nullptr || bytes == 0) return false;
    /* Se normaliza aqui aunque `NtAllocateVirtualMemory` redondee sola: asi
     * esta funcion significa lo mismo en Windows y en POSIX por CONTRATO, y no
     * porque dos implementaciones ajenas coincidan.  Ver `page_range`. */
    page_range(addr, bytes);
    PVOID base = addr;
    SIZE_T size = bytes;
    return NtAllocateVirtualMemory(VESTA_NT_SELF, &base, 0, &size, MEM_COMMIT,
                                   kWinProt[prot_index(prot)]) >= 0;
}

void *os_alloc(size_t bytes, OsProt prot) noexcept {
    if (bytes == 0) return nullptr;
    PVOID base = nullptr;
    SIZE_T size = round_up(bytes, os_page_size());
    return NtAllocateVirtualMemory(VESTA_NT_SELF, &base, 0, &size,
                                   MEM_COMMIT | MEM_RESERVE,
                                   kWinProt[prot_index(prot)]) >= 0
               ? base
               : nullptr;
}

void *os_alloc_near(size_t bytes, OsProt prot, const void *anchor,
                    size_t window, OsNearScan *scan) noexcept {
    if (scan != nullptr) *scan = OsNearScan{0, 0};
    if (bytes == 0) return nullptr;
    if (anchor == nullptr) return os_alloc(bytes, prot);

    /* Al grano de RESERVA, que en Windows es de 64 KiB y no la pagina: una
     * reserva solo puede empezar ahi, y pedir en medio la redondea hacia abajo
     * -- o sea que se acabaria mirando una direccion y reservando otra. */
    const uintptr_t grain = os_reserve_granularity();
    const uintptr_t base = reinterpret_cast<uintptr_t>(anchor) & ~(grain - 1);
    const uintptr_t low = (base > window) ? (base - window) : grain;
    const uintptr_t high =
        (base + window < base) ? ~uintptr_t(0) : (base + window);
    const size_t want = round_up(bytes, os_page_size());

    /* SE RECORREN LAS REGIONES, no se prueban direcciones.  Probar `base +- 2^k`
     * -- treinta puntos de la ventana -- es lo que habia antes en quien llama, y
     * basta una reserva grande por en medio para que los treinta esten
     * ocupados; entonces se caia a la eleccion del sistema y el bloque acababa
     * a 16 GiB.  Preguntar cuesta una consulta por region y solo al reservar. */
    /* SE BUSCA EL HUECO MAS CERCANO, no el primero.  Recorrer de un extremo al
     * otro y quedarse con el primero que quepa deja el bloque en el BORDE de la
     * ventana -- medido: a 1.920 MiB de un ancla que tenia hueco mucho mas
     * cerca --, y ahi el margen es cero: el desplazamiento se mide contra CADA
     * dato, no contra el ancla, asi que cualquiera un poco mas alla ya no
     * alcanza.  Se recorre entera y se elige por distancia. */
    uintptr_t best = 0;
    uintptr_t best_dist = ~uintptr_t(0);
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t probe = low; probe < high;) {
        if (NtQueryVirtualMemory(VESTA_NT_SELF, reinterpret_cast<PVOID>(probe),
                                 0 /* MemoryBasicInformation */, &mbi,
                                 sizeof(mbi), nullptr) < 0)
            break;
        const uintptr_t region_begin =
            reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t region_end = region_begin + mbi.RegionSize;
        if (scan != nullptr) ++scan->regions;
        if (mbi.State == MEM_FREE) {
            const uintptr_t from = (region_begin < probe) ? probe : region_begin;
            const uintptr_t first = (from + grain - 1) & ~(grain - 1);
            if (scan != nullptr && first < high && region_end > first) {
                const size_t free_here = size_t(region_end - first);
                if (free_here > scan->largest_free)
                    scan->largest_free = free_here;
            }
            if (first < high && region_end > first &&
                (region_end - first) >= want) {
                /* Dentro de la region, el sitio mas cercano al ancla: pegado al
                 * principio si la region esta por encima, pegado al final si
                 * esta por debajo, y el propio ancla si lo contiene. */
                uintptr_t at = first;
                if (base > first) {
                    const uintptr_t last = (region_end - want) & ~(grain - 1);
                    at = (base < last) ? (base & ~(grain - 1)) : last;
                    if (at < first) at = first;
                }
                const uintptr_t dist = at > base ? (at - base) : (base - at);
                if (dist < best_dist) {
                    best_dist = dist;
                    best = at;
                }
            }
        }
        /* Avanzar SIEMPRE, aunque la consulta devuelva una region rara: con un
         * tamano cero esto se queda dando vueltas. */
        probe = (region_end > probe) ? region_end : (probe + grain);
    }
    if (best != 0) {
        PVOID p = reinterpret_cast<PVOID>(best);
        SIZE_T size = want;
        /* Puede fallar aunque la region dijera libre: entre la consulta y esto,
         * otro hilo pudo tomarla.  Entonces se cede al sistema en vez de
         * insistir -- es raro, y un bucle de reintento aqui es una espera sin
         * cota --, y quien llama comprueba la distancia igualmente. */
        if (NtAllocateVirtualMemory(VESTA_NT_SELF, &p, 0, &size,
                                    MEM_COMMIT | MEM_RESERVE,
                                    kWinProt[prot_index(prot)]) >= 0)
            return p;
    }
    return nullptr; // no habia sitio EN LA VENTANA; no es quedarse sin memoria
}

void os_free(void *addr, size_t bytes) noexcept {
    (void)bytes; // con MEM_RELEASE hay que pasar cero; el tamano no se usa
    release_range(addr);
}

bool os_protect(void *addr, size_t bytes, OsProt prot) noexcept {
    if (addr == nullptr || bytes == 0) return false;
    page_range(addr, bytes);
    PVOID base = addr;
    SIZE_T size = bytes;
    ULONG previous = 0;
    return NtProtectVirtualMemory(VESTA_NT_SELF, &base, &size,
                                  kWinProt[prot_index(prot)], &previous) >= 0;
}

bool os_decommit(void *addr, size_t bytes) noexcept {
    if (addr == nullptr || bytes == 0) return false;
    /* `MEM_DECOMMIT` devuelve las paginas pero CONSERVA la reserva, que es
     * justo lo que hace falta: el rango sigue siendo nuestro y se puede volver
     * a comprometer sin que nadie se meta en medio. */
    page_range(addr, bytes);
    PVOID base = addr;
    SIZE_T size = bytes;
    return NtFreeVirtualMemory(VESTA_NT_SELF, &base, &size, MEM_DECOMMIT) >= 0;
}

void os_release(void *addr, size_t bytes) noexcept {
    (void)bytes; // con MEM_RELEASE hay que pasar cero; el tamano no se usa
    release_range(addr);
}

OsProcessMemory os_process_memory() noexcept {
    OsProcessMemory m;
    /* `GetProcessMemoryInfo` no hace mas que esto, y de paso obligaba a pedir
     * la version 2 de psapi para que no arrastrara otra biblioteca al enlace.
     * Preguntando directo, esa dependencia desaparece del todo. */
    VestaVmCounters vm;
    if (NtQueryInformationProcess(VESTA_NT_SELF, 3 /* ProcessVmCounters */, &vm,
                                  sizeof(vm), nullptr) >= 0) {
        m.working_set = vm.WorkingSetSize;
        m.working_set_peak = vm.PeakWorkingSetSize;
        /* `PrivateUsage` es lo comprometido POR ESTE proceso; `PagefileUsage`
         * mide lo mismo en la practica pero esta documentado como historico. */
        m.commit = vm.PrivateUsage;
        m.commit_peak = vm.PeakPagefileUsage;
    }
    return m;
}

size_t os_range_memory(const void *base, size_t bytes) noexcept {
    if (base == nullptr || bytes == 0) return 0;

    const uintptr_t begin = reinterpret_cast<uintptr_t>(base);
    const uintptr_t end = begin + bytes;
    size_t total = 0;

    /* Se pregunta REGION a REGION, no pagina a pagina.  Cada respuesta trae el
     * tamano del tramo entero que comparte estado, asi que un rango de 256 GiB
     * apenas reservado se recorre en unas pocas consultas en vez de en sesenta
     * y siete millones. */
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t probe = begin; probe < end;) {
        if (NtQueryVirtualMemory(VESTA_NT_SELF, reinterpret_cast<PVOID>(probe),
                                 0 /* MemoryBasicInformation */, &mbi,
                                 sizeof(mbi), nullptr) < 0)
            break;
        const uintptr_t region_begin =
            reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        uintptr_t region_end = region_begin + mbi.RegionSize;
        if (region_end <= probe) break; // sin avance: el sistema no sabe mas

        if (mbi.State == MEM_COMMIT) {
            /* Se cuenta solo la PARTE que cae dentro: la primera region suele
             * empezar antes del rango y la ultima acabar despues. */
            const uintptr_t from = probe > region_begin ? probe : region_begin;
            const uintptr_t to = region_end < end ? region_end : end;
            if (to > from) total += static_cast<size_t>(to - from);
        }
        probe = region_end;
    }
    return total;
}

OsSystemMemory os_system_memory() noexcept {
    OsSystemMemory m;
    MEMORYSTATUSEX st;
    st.dwLength = sizeof(st);
    if (GlobalMemoryStatusEx(&st)) {
        m.physical_total = st.ullTotalPhys;
        m.physical_available = st.ullAvailPhys;
        m.address_space_total = st.ullTotalVirtual;
        m.address_space_free = st.ullAvailVirtual;
    }
    return m;
}

/* La cabecera DOS del propio modulo, que el enlazador coloca al principio de
 * la imagen: su direccion ES la base.  Se prefiere a `GetModuleHandle` porque
 * no es una llamada -- es un simbolo -- y porque no puede fallar. */
extern "C" IMAGE_DOS_HEADER __ImageBase;

const void *os_module_base() noexcept { return &__ImageBase; }

/* `SwitchToThread` is preferred over `Sleep(0)`: it yields to any READY thread
 * on this processor even at a lower priority, which is exactly the case that
 * matters -- the lock holder that cannot get a core.  `Sleep(0)` only yields to
 * threads of EQUAL or higher priority, so it can return immediately without
 * having let anybody run. */
void os_yield() noexcept { SwitchToThread(); }


#else // ---------------------------------------------------------------- POSIX

// ---------------------------------------------------------------------------
//  Llamadas al sistema DIRECTAS, sin pasar por la biblioteca C
// ---------------------------------------------------------------------------
//
// POR QUE.  No por velocidad: esta medido y no la hay.  El envoltorio de glibc
// cuesta unos 11 ns -- se ve aislandolo con `getpid`, la llamada mas barata que
// existe --, pero contra un `mmap` de 1,8 us eso es el 0,6%, y en la medida
// cara a cara glibc incluso salia por delante, dentro del ruido:
//
//     mmap + munmap de 64 KiB      glibc 1.793 ns    a mano 1.826 ns
//     getpid                       glibc    82 ns    a mano    71 ns
//
// La razon es otra: **asi esto funciona SIN biblioteca C**.  Un asignador que
// emite la instruccion por su cuenta sirve en modo nucleo y en freestanding,
// que es una direccion declarada del proyecto.  Y de paso quita una dependencia
// de `errno`, que es una variable por hilo a la que en algunas configuraciones
// se llega por una llamada.
//
// SOLO x86-64 y solo Linux.  Fuera de ahi se usa la biblioteca C, y eso no es
// una renuncia: los numeros de llamada son de Linux y cambian por arquitectura,
// y la convencion de i386 para SEIS argumentos pasa el ultimo por `ebp`, que
// choca con el puntero de marco y es fragil de escribir.  Anadir una
// arquitectura es anadir su tabla aqui; mientras no este, funciona igual.
#if defined(__linux__) && defined(__x86_64__)
#define VESTA_ALLOC_RAW_SYSCALL 1

namespace {

/* Numeros de llamada de Linux en x86-64.  Escritos a mano a proposito: cogerlos
 * de `<sys/syscall.h>` volveria a atar esto a las cabeceras de la biblioteca C,
 * que es justo lo que se quiere evitar. */
constexpr long kSysMmap = 9;
constexpr long kSysMprotect = 10;
constexpr long kSysMunmap = 11;
constexpr long kSysSchedYield = 24;
constexpr long kSysMadvise = 28;

/**
 * @brief Emite la instruccion `syscall` con hasta seis argumentos.
 *
 * La convencion de Linux en x86-64: el numero en `rax`, los argumentos en
 * `rdi`, `rsi`, `rdx`, `r10`, `r8` y `r9` -- ojo, `r10` y no `rcx`, porque la
 * instruccion PISA `rcx` y `r11` para guardar la direccion de retorno y las
 * banderas --, y el resultado en `rax`.
 */
inline long raw_syscall(long n, long a = 0, long b = 0, long c = 0, long d = 0,
                        long e = 0, long f = 0) noexcept {
    long ret;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8),
                       "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}

/**
 * @brief Si lo devuelto por una llamada es un error.
 *
 * Linux devuelve el error como `-errno` en el rango [-4095, -1].  No hay
 * ninguna direccion valida ahi, asi que la comprobacion no puede confundir un
 * puntero bueno con un fallo.  Y no se toca `errno`: quien llama a esta capa
 * mira el valor de retorno, no una variable global.
 */
inline bool sys_failed(long ret) noexcept {
    return static_cast<unsigned long>(ret) >= static_cast<unsigned long>(-4095);
}

inline void *sys_mmap(void *addr, size_t len, int prot, int flags) noexcept {
    const long r = raw_syscall(kSysMmap, reinterpret_cast<long>(addr),
                               long(len), prot, flags, -1, 0);
    return sys_failed(r) ? nullptr : reinterpret_cast<void *>(r);
}

inline bool sys_mprotect(void *addr, size_t len, int prot) noexcept {
    return !sys_failed(raw_syscall(kSysMprotect, reinterpret_cast<long>(addr),
                                   long(len), prot));
}

inline void sys_munmap(void *addr, size_t len) noexcept {
    raw_syscall(kSysMunmap, reinterpret_cast<long>(addr), long(len));
}

inline bool sys_madvise(void *addr, size_t len, int advice) noexcept {
    return !sys_failed(raw_syscall(kSysMadvise, reinterpret_cast<long>(addr),
                                   long(len), advice));
}

} // namespace

#else // ------------------- el resto: por la biblioteca C

namespace {

inline void *sys_mmap(void *addr, size_t len, int prot, int flags) noexcept {
    void *p = mmap(addr, len, prot, flags, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}
inline bool sys_mprotect(void *addr, size_t len, int prot) noexcept {
    return mprotect(addr, len, prot) == 0;
}
inline void sys_munmap(void *addr, size_t len) noexcept { munmap(addr, len); }
inline bool sys_madvise(void *addr, size_t len, int advice) noexcept {
    return madvise(addr, len, advice) == 0;
}

} // namespace

#endif

/**
 * @brief La misma tabla que en Windows, con las constantes de aqui.
 *
 * Aqui el mapeo si es un OR de bits directo, asi que la tabla no ahorra ramas
 * -- no las habia --, pero se mantiene por dos motivos: que las dos ramas del
 * fichero se lean igual, y que el compilador pliegue el resultado cuando los
 * permisos son constantes, que es lo que pasa en casi todos los sitios.
 */
constexpr int kPosixProt[8] = {
    PROT_NONE,                            // 000  ---
    PROT_READ,                            // 001  R
    PROT_WRITE,                           // 010  -W-
    PROT_READ | PROT_WRITE,               // 011  RW
    PROT_EXEC,                            // 100  --X
    PROT_READ | PROT_EXEC,                // 101  R-X
    PROT_WRITE | PROT_EXEC,               // 110  -WX
    PROT_READ | PROT_WRITE | PROT_EXEC,   // 111  RWX
};

void *os_reserve(size_t bytes) noexcept {
    if (bytes == 0) return nullptr;
    /* `PROT_NONE` mas `MAP_NORESERVE` es el equivalente de apalabrar sin
     * gastar: el nucleo no apunta nada contra el limite de memoria
     * comprometida y las paginas no existen hasta que se pidan. */
    return sys_mmap(nullptr, round_up(bytes, os_page_size()), PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE);
}

bool os_commit(void *addr, size_t bytes, OsProt prot) noexcept {
    if (addr == nullptr || bytes == 0) return false;
    page_range(addr, bytes); // ver `page_range`: sin esto falla si no es pagina
    return sys_mprotect(addr, bytes, kPosixProt[prot_index(prot)]);
}

void *os_alloc(size_t bytes, OsProt prot) noexcept {
    if (bytes == 0) return nullptr;
    return sys_mmap(nullptr, round_up(bytes, os_page_size()),
                    kPosixProt[prot_index(prot)],
                    MAP_PRIVATE | MAP_ANONYMOUS);
}

void *os_alloc_near(size_t bytes, OsProt prot, const void *anchor,
                    size_t window, OsNearScan *scan) noexcept {
    /* AQUI NO SE RECORRE NADA, asi que no hay regiones que contar: se le pide
     * al nucleo y se mira donde cayo.  Se deja en cero en vez de inventar una
     * cifra -- un informe que dice "vio 0 regiones" en ELF esta diciendo la
     * verdad: el camino es otro, y quien lo lea tiene que saberlo. */
    if (scan != nullptr) *scan = OsNearScan{0, 0};
    if (bytes == 0) return nullptr;
    if (anchor == nullptr) return os_alloc(bytes, prot);

    const size_t page = os_page_size();
    const size_t want = round_up(bytes, page);
    const uintptr_t base = reinterpret_cast<uintptr_t>(anchor) & ~(page - 1);

    /* AQUI SE PIDE, no se recorre el mapa.  `mmap` acepta una direccion
     * preferida SIN `MAP_FIXED`: si esa esta ocupada el nucleo elige otra por su
     * cuenta y no pisa nada, asi que se prueba y se mira DONDE cayo.  Es la
     * diferencia con Windows, donde una reserva en la direccion pedida o sale o
     * falla, y por eso alli hay que preguntar primero.
     *
     * `MAP_FIXED` haria justo lo que no se quiere: entregar la direccion
     * pisando lo que hubiera, que en un proceso vivo es corromper a otro.
     *
     * Se avanza a saltos de un dieciseisavo de la ventana a los dos lados, para
     * no depender de que el primer intento acierte y sin recorrer pagina a
     * pagina una ventana de gigas. */
    const size_t step = (window / 16) < page ? page : (window / 16);
    for (size_t off = 0; off <= window; off += step) {
        for (int sign = 0; sign < 2; ++sign) {
            const uintptr_t at = sign == 0 ? (base + off)
                                           : (base > off ? base - off : 0);
            if (at == 0) continue;
            void *p = sys_mmap(reinterpret_cast<void *>(at), want,
                               kPosixProt[prot_index(prot)],
                               MAP_PRIVATE | MAP_ANONYMOUS);
            if (p == nullptr) continue;
            /* Lo que decide es DONDE cayo, no que la llamada saliera bien: sin
             * `MAP_FIXED` el nucleo puede haberlo puesto en cualquier sitio, y
             * un bloque lejos no sirve para lo que se pidio.  Si no vale se
             * suelta y se sigue; quedarselo seria contestar que si a una
             * pregunta que era sobre la distancia. */
            const uintptr_t got = reinterpret_cast<uintptr_t>(p);
            const uintptr_t dist = got > base ? (got - base) : (base - got);
            if (dist <= window) return p;
            sys_munmap(p, want);
            if (off == 0) break; // el cero es el mismo sitio por los dos lados
        }
    }
    return nullptr; // no habia sitio EN LA VENTANA; no es quedarse sin memoria
}

void os_free(void *addr, size_t bytes) noexcept {
    if (addr != nullptr) sys_munmap(addr, round_up(bytes, os_page_size()));
}

bool os_protect(void *addr, size_t bytes, OsProt prot) noexcept {
    if (addr == nullptr || bytes == 0) return false;
    /* `page_range`, not just rounding the size up: `mprotect` REQUIRES a
     * page-aligned address and `VirtualProtect` does not -- it aligns on its
     * own.  This rounded the size but not the address, so the very same call
     * worked on Windows and failed on Linux with EINVAL.
     *
     * The `operator new` patch (`call_site.cpp`) uncovered it: it protects five
     * bytes in the middle of a function, and on Linux none of the eight patches
     * were installed.  This is what `os_commit` and `os_decommit` already do,
     * and for the same reason -- this layer meaning THE SAME THING on both
     * systems is exactly what is asked of it. */
    page_range(addr, bytes);
    return sys_mprotect(addr, bytes, kPosixProt[prot_index(prot)]);
}

bool os_decommit(void *addr, size_t bytes) noexcept {
    if (addr == nullptr || bytes == 0) return false;
    page_range(addr, bytes);
    const size_t n = bytes;
    /* Dos pasos, y los dos hacen falta: `MADV_DONTNEED` es lo que de verdad
     * suelta las paginas, y volver a `PROT_NONE` es lo que hace que tocarlas
     * falle en vez de devolver ceros en silencio.  Sin lo segundo, un uso
     * despues de soltar no daria error, daria otro valor. */
    const bool freed = sys_madvise(addr, n, MADV_DONTNEED);
    return sys_mprotect(addr, n, PROT_NONE) && freed;
}

void os_release(void *addr, size_t bytes) noexcept {
    if (addr != nullptr) sys_munmap(addr, round_up(bytes, os_page_size()));
}

namespace {

/**
 * @brief Lee los siete numeros de `/proc/self/statm`, en PAGINAS.
 * @return Cuantos consiguio leer.
 *
 * POR QUE `statm` Y NO `status`.  Los dos dan lo mismo, pero `status` son ~1,4
 * KiB de texto con etiquetas y hay que ESCANEARLO buscando cada clave -- y una
 * pasada por clave --, mientras que `statm` son siete numeros separados por
 * espacios en unos cincuenta bytes.  Aqui se lee UNA vez y salen todos.
 *
 * En Linux no hay forma de saber la memoria residente sin pasar por `/proc`: el
 * nucleo no la expone por ninguna otra via.  Lo que si se puede evitar es el
 * PICO, que sale de `getrusage` sin tocar ningun fichero.
 *
 * A mano y con buffer fijo: nada de flujos ni de cadenas, porque esto tiene que
 * poder llamarse desde un sitio sin memoria.
 */
unsigned read_statm(uint64_t out[7]) noexcept {
    for (unsigned i = 0; i < 7; ++i)
        out[i] = 0;
    const int fd = open("/proc/self/statm", O_RDONLY);
    if (fd < 0) return 0;
    char buf[128];
    const ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';

    unsigned got = 0;
    const char *p = buf;
    while (got < 7) {
        while (*p == ' ' || *p == '\t' || *p == '\n')
            ++p;
        if (*p < '0' || *p > '9') break;
        uint64_t v = 0;
        while (*p >= '0' && *p <= '9')
            v = v * 10 + uint64_t(*p++ - '0');
        out[got++] = v;
    }
    return got;
}

/**
 * @brief Lector de lineas con buffer fijo, sin reservar nada.
 *
 * `/proc/self/smaps` son decenas de KiB de texto y hay que recorrerlo entero,
 * pero no cabe de una lectura ni conviene que quepa: se lee por trozos y las
 * lineas se van sacando de ahi.  Sin flujos y sin cadenas, por lo mismo que
 * `read_statm`: esto tiene que poder llamarse desde un sitio sin memoria.
 */
struct LineReader {
    int fd = -1;
    char buf[4096];
    size_t have = 0; // bytes validos en el buffer
    size_t pos = 0;  // siguiente byte sin consumir

    bool open_file(const char *path) noexcept {
        fd = open(path, O_RDONLY);
        return fd >= 0;
    }

    void close_file() noexcept {
        if (fd >= 0) close(fd);
        fd = -1;
    }

    /**
     * @brief Copia la siguiente linea, sin el salto, en `out`.
     * @return false cuando se acabo el fichero.
     *
     * Una linea mas larga que `cap` se TRUNCA en vez de partirse en dos: las
     * que interesan son cortas, y partir una daria una linea falsa que podria
     * parecer otra cosa.
     */
    bool next(char *out, size_t cap) noexcept {
        size_t n = 0;
        for (;;) {
            if (pos == have) {
                const ssize_t r = read(fd, buf, sizeof(buf));
                if (r <= 0) {
                    out[n] = '\0';
                    return n != 0;
                }
                have = size_t(r);
                pos = 0;
            }
            const char c = buf[pos++];
            if (c == '\n') {
                out[n] = '\0';
                return true;
            }
            if (n + 1 < cap) out[n++] = c;
        }
    }
};

/// Un digito hexadecimal, o 16 si no lo es.
inline unsigned hex_digit(char c) noexcept {
    if (c >= '0' && c <= '9') return unsigned(c - '0');
    if (c >= 'a' && c <= 'f') return unsigned(c - 'a') + 10;
    if (c >= 'A' && c <= 'F') return unsigned(c - 'A') + 10;
    return 16;
}

/**
 * @brief Reconoce la cabecera de una region: `<inicio>-<fin> <permisos> ...`.
 * @return false si la linea es un campo de la region anterior.
 */
bool parse_map_header(const char *line, uintptr_t *begin,
                      uintptr_t *end) noexcept {
    const char *p = line;
    uintptr_t lo = 0;
    unsigned digits = 0;
    for (unsigned d; (d = hex_digit(*p)) < 16; ++p, ++digits)
        lo = (lo << 4) | d;
    if (digits == 0 || *p != '-') return false;
    ++p;
    uintptr_t hi = 0;
    digits = 0;
    for (unsigned d; (d = hex_digit(*p)) < 16; ++p, ++digits)
        hi = (hi << 4) | d;
    if (digits == 0 || *p != ' ') return false;
    *begin = lo;
    *end = hi;
    return true;
}

/**
 * @brief Lee `<clave>: <numero> kB` y devuelve el numero, en KiB.
 * @return false si la linea no es esa clave.
 */
bool parse_kib_field(const char *line, const char *key,
                     uint64_t *out) noexcept {
    const char *p = line;
    const char *k = key;
    while (*k != '\0') {
        if (*p != *k) return false;
        ++p;
        ++k;
    }
    while (*p == ' ' || *p == '\t')
        ++p;
    if (*p < '0' || *p > '9') return false;
    uint64_t v = 0;
    while (*p >= '0' && *p <= '9')
        v = v * 10 + uint64_t(*p++ - '0');
    *out = v;
    return true;
}

} // namespace

size_t os_range_memory(const void *base, size_t bytes) noexcept {
    if (base == nullptr || bytes == 0) return 0;

    const uintptr_t begin = reinterpret_cast<uintptr_t>(base);
    const uintptr_t end = begin + bytes;

    LineReader lr;
    if (!lr.open_file("/proc/self/smaps")) return 0;

    /* SE SUMA `Rss`, que es lo unico que este sistema sabe contestar: aqui no
     * hay "comprometido" -- un mapeo se respalda segun se toca --, asi que lo
     * que cuesta el rango es lo que tiene residente.  La cabecera lo dice sin
     * taparlo; las dos cifras no son la misma medida. */
    size_t total = 0;
    uintptr_t vma_begin = 0;
    uintptr_t vma_end = 0;
    bool touches = false; // la region actual cae dentro del rango
    char line[256];
    while (lr.next(line, sizeof(line))) {
        if (parse_map_header(line, &vma_begin, &vma_end)) {
            touches = vma_begin < end && vma_end > begin;
            continue;
        }
        if (!touches) continue;
        uint64_t kib;
        if (!parse_kib_field(line, "Rss:", &kib)) continue;

        /* Una region puede asomar por los bordes del rango, y `Rss` es de la
         * region ENTERA.  Lo normal es que caiga dentro del todo -- cada
         * `mprotect` parte la region por donde se pidio --, y cuando no, se
         * reparte a prorrata: es lo unico que se puede decir sin preguntar
         * pagina por pagina, que costaria un bitmap de 64 MiB por cada 256 GiB
         * de rango. */
        const uintptr_t from = vma_begin > begin ? vma_begin : begin;
        const uintptr_t to = vma_end < end ? vma_end : end;
        const uint64_t rss = kib * 1024;
        const uint64_t span = uint64_t(vma_end - vma_begin);
        const uint64_t part = uint64_t(to - from);
        total += size_t(span == part ? rss : (rss * part) / span);
        touches = false; // `Rss` sale una vez por region; el resto sobra
    }
    lr.close_file();
    return total;
}

OsProcessMemory os_process_memory() noexcept {
    OsProcessMemory m;

    /* El PICO sin tocar disco: una llamada al sistema y ya.  `ru_maxrss` va en
     * KiB en Linux y en bytes en macOS; aqui solo se compila para el primero. */
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0)
        m.working_set_peak = uint64_t(ru.ru_maxrss) * 1024;

    /* Y lo actual, de una sola lectura.  Los campos de `statm`, en paginas:
     *   0 tamano total    1 RESIDENTE    2 compartidas    3 texto
     *   4 (sin usar)      5 DATOS+pila   6 (sin usar) */
    uint64_t f[7];
    if (read_statm(f) >= 6) {
        const uint64_t ps = os_page_size();
        m.working_set = f[1] * ps;
        m.commit = f[5] * ps;
    }
    /* `commit_peak` se queda a cero: el pico de memoria VIRTUAL solo esta en
     * `/proc/self/status`, y no compensa leer 1,4 KiB de texto por un dato que
     * casi nadie mira.  Cero significa "este sistema no lo sabe decir", que es
     * lo que promete la cabecera. */
    return m;
}

OsSystemMemory os_system_memory() noexcept {
    OsSystemMemory m;
    const long total_pages = sysconf(_SC_PHYS_PAGES);
    const long free_pages = sysconf(_SC_AVPHYS_PAGES);
    const uint64_t ps = os_page_size();
    if (total_pages > 0) m.physical_total = uint64_t(total_pages) * ps;
    if (free_pages > 0) m.physical_available = uint64_t(free_pages) * ps;
    /* Cuanto puede direccionar el proceso: si hay un limite puesto se respeta,
     * y si no, lo que da la arquitectura.  No hay forma portable de saber
     * cuanto queda SIN apalabrar, asi que ese campo se deja a cero -- que
     * significa "este sistema no lo sabe decir", no "cero". */
    struct rlimit rl;
    if (getrlimit(RLIMIT_AS, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
        m.address_space_total = uint64_t(rl.rlim_cur);
    else
        m.address_space_total = sizeof(void *) >= 8 ? (uint64_t(128) << 40)
                                                    : (uint64_t(3) << 30);
    return m;
}

/**
 * @brief La base del ejecutable, leida de `/proc/self/maps`.
 *
 * POR QUE ASI Y NO CON `dladdr`.  Porque `dladdr` obliga a enlazar con `dl`, y
 * esta libreria se distribuye aparte y no arrastra dependencias que no
 * necesita.  Esto corre UNA vez, al volcar, que es cuando el proceso ya ha
 * terminado su trabajo -- ahi una lectura de fichero no le quita nada a nadie.
 *
 * La primera linea del mapa es la primera region mapeada de la imagen, y su
 * direccion de inicio es la base.
 */
const void *os_module_base() noexcept {
    static const void *cached = [] () -> const void * {
        FILE *f = std::fopen("/proc/self/maps", "r");
        if (f == nullptr) return nullptr;
        char line[512];
        const void *base = nullptr;
        if (std::fgets(line, sizeof(line), f) != nullptr) {
            unsigned long long start = 0;
            if (std::sscanf(line, "%llx", &start) == 1)
                base = reinterpret_cast<const void *>(uintptr_t(start));
        }
        std::fclose(f);
        return base;
    }();
    return cached;
}

void os_yield() noexcept {
#if defined(VESTA_ALLOC_RAW_SYSCALL)
    raw_syscall(kSysSchedYield); // without going through the C library
#else
    sched_yield();
#endif
}


#endif

void *os_reserve_largest(size_t max, size_t min, size_t *got) noexcept {
    if (got != nullptr) *got = 0;
    if (min > max) return nullptr;
    /* A la mitad cada vez, no en pasos fijos: el numero de intentos queda
     * logaritmico, asi que se puede pedir muchisimo sin que rendirse cueste. */
    for (size_t n = max; n >= min; n /= 2) {
        void *p = os_reserve(n);
        if (p != nullptr) {
            if (got != nullptr) *got = n;
            return p;
        }
        if (n == min) break; // la division no llegaria nunca a `min` exacto
    }
    return nullptr;
}

} // namespace util

/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
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

#include "util/os_memory.h"

#include <atomic>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
/* Con la version 2 de la API, `GetProcessMemoryInfo` se resuelve a
 * `K32GetProcessMemoryInfo`, que vive en `kernel32.dll` -- ya enlazada -- en
 * vez de en `psapi.dll`.  Asi preguntar por la memoria del proceso no anade una
 * biblioteca al enlace de los treinta y tantos objetivos del proyecto. */
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace util {

namespace detail {

/* Atomicos sueltos y no un estatico de funcion: un estatico de funcion lleva
 * variable de guarda, y las guardas en este toolchain ya costaron un cuelgue
 * (ver `util/thread_slot.h`).  Dos hilos que lleguen a la vez preguntan los dos
 * y escriben lo mismo, que no es un problema. */
std::atomic<size_t> g_page_size{0};
std::atomic<size_t> g_granularity{0};

void query_os_sizes() noexcept {
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_page_size.store(si.dwPageSize, std::memory_order_relaxed);
    g_granularity.store(si.dwAllocationGranularity, std::memory_order_relaxed);
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

} // namespace

/* `os_page_size` y `os_reserve_granularity` estan EN LINEA en la cabecera: son
 * una lectura relajada y una rama predicha, y las llama `vm::allocate_memory`
 * en cada reserva para redondear a paginas.  Aqui solo vive lo que hay que
 * preguntarle al sistema, que pasa una vez. */

#if defined(_WIN32)

void *os_reserve(size_t bytes) noexcept {
    if (bytes == 0) return nullptr;
    /* `PAGE_READWRITE` en una reserva no entrega nada: es el permiso que
     * tendran las paginas cuando se comprometan.  Mientras no se comprometan,
     * tocarlas falla igual. */
    return VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_READWRITE);
}

bool os_commit(void *addr, size_t bytes) noexcept {
    if (addr == nullptr || bytes == 0) return false;
    return VirtualAlloc(addr, bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

bool os_decommit(void *addr, size_t bytes) noexcept {
    if (addr == nullptr || bytes == 0) return false;
    /* `MEM_DECOMMIT` devuelve las paginas pero CONSERVA la reserva, que es
     * justo lo que hace falta: el rango sigue siendo nuestro y se puede volver
     * a comprometer sin que nadie se meta en medio. */
    return VirtualFree(addr, bytes, MEM_DECOMMIT) != 0;
}

void os_release(void *addr, size_t bytes) noexcept {
    (void)bytes; // con MEM_RELEASE hay que pasar cero; el tamano no se usa
    if (addr != nullptr) VirtualFree(addr, 0, MEM_RELEASE);
}

OsProcessMemory os_process_memory() noexcept {
    OsProcessMemory m;
    PROCESS_MEMORY_COUNTERS_EX pmc;
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc),
                             sizeof(pmc))) {
        m.working_set = pmc.WorkingSetSize;
        m.working_set_peak = pmc.PeakWorkingSetSize;
        /* `PrivateUsage` es lo comprometido POR ESTE proceso; `PagefileUsage`
         * mide lo mismo en la practica pero esta documentado como historico. */
        m.commit = pmc.PrivateUsage;
        m.commit_peak = pmc.PeakPagefileUsage;
    }
    return m;
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

#else // ---------------------------------------------------------------- POSIX

void *os_reserve(size_t bytes) noexcept {
    if (bytes == 0) return nullptr;
    /* `PROT_NONE` mas `MAP_NORESERVE` es el equivalente de apalabrar sin
     * gastar: el nucleo no apunta nada contra el limite de memoria
     * comprometida y las paginas no existen hasta que se pidan. */
    void *p = mmap(nullptr, round_up(bytes, os_page_size()), PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}

bool os_commit(void *addr, size_t bytes) noexcept {
    if (addr == nullptr || bytes == 0) return false;
    return mprotect(addr, round_up(bytes, os_page_size()),
                    PROT_READ | PROT_WRITE) == 0;
}

bool os_decommit(void *addr, size_t bytes) noexcept {
    if (addr == nullptr || bytes == 0) return false;
    const size_t n = round_up(bytes, os_page_size());
    /* Dos pasos, y los dos hacen falta: `MADV_DONTNEED` es lo que de verdad
     * suelta las paginas, y volver a `PROT_NONE` es lo que hace que tocarlas
     * falle en vez de devolver ceros en silencio.  Sin lo segundo, un uso
     * despues de soltar no daria error, daria otro valor. */
    const bool freed = madvise(addr, n, MADV_DONTNEED) == 0;
    return mprotect(addr, n, PROT_NONE) == 0 && freed;
}

void os_release(void *addr, size_t bytes) noexcept {
    if (addr != nullptr) munmap(addr, round_up(bytes, os_page_size()));
}

namespace {

/**
 * @brief Lee un numero de `/proc/self/<que>` etiquetado con @p clave, en KiB.
 *
 * A mano y con buffer fijo: nada de flujos ni de cadenas, porque este fichero
 * tiene que poder llamarse desde un sitio sin memoria.  Devuelve 0 si no esta.
 */
uint64_t proc_status_kib(const char *key) noexcept {
    const int fd = open("/proc/self/status", O_RDONLY);
    if (fd < 0) return 0;
    char buf[4096];
    const ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';

    size_t klen = 0;
    while (key[klen] != '\0')
        ++klen;
    for (const char *p = buf; *p != '\0';) {
        bool match = true;
        for (size_t i = 0; i < klen; ++i)
            if (p[i] != key[i]) {
                match = false;
                break;
            }
        if (match) {
            const char *q = p + klen;
            while (*q == ' ' || *q == '\t' || *q == ':')
                ++q;
            uint64_t v = 0;
            while (*q >= '0' && *q <= '9')
                v = v * 10 + uint64_t(*q++ - '0');
            return v * 1024; // el fichero da KiB
        }
        while (*p != '\n' && *p != '\0')
            ++p;
        if (*p == '\n') ++p;
    }
    return 0;
}

} // namespace

OsProcessMemory os_process_memory() noexcept {
    OsProcessMemory m;
    m.working_set = proc_status_kib("VmRSS");
    m.working_set_peak = proc_status_kib("VmHWM");
    m.commit = proc_status_kib("VmData");
    /* `ru_maxrss` esta en KiB en Linux y en bytes en macOS.  Solo se usa como
     * respaldo de `VmHWM`, que es exacto donde existe. */
    if (m.working_set_peak == 0) {
        struct rusage ru;
        if (getrusage(RUSAGE_SELF, &ru) == 0)
            m.working_set_peak = uint64_t(ru.ru_maxrss) * 1024;
    }
    m.commit_peak = proc_status_kib("VmPeak");
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

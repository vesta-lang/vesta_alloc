/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file util/symbols/link_map.cpp
 * @brief
 * \~english The linker's map, read.  Why, in the header.
 * \~spanish El mapa del enlazador, leido.  El porque, en la cabecera.
 * \~
 *
 * \~english
 * NOTHING HERE GOES THROUGH THE ALLOCATOR, and the STL is not an exception --
 * it is the CASE.  This library hooks `malloc` and its family, so a
 * `std::vector` here does not avoid the allocator, it IS a call into it: the
 * reader would land inside the very figures it exists to explain, and quietly.
 * So every byte comes from `os_alloc`, which asks the system directly.
 *
 * \~spanish
 * NADA DE AQUI PASA POR EL ASIGNADOR, y el STL no es una excepcion -- es EL
 * CASO.  Esta libreria engancha `malloc` y su familia, asi que un
 * `std::vector` aqui no evita el asignador, ES una llamada a el: el lector
 * acabaria dentro de las mismas cifras que existe para explicar, y calladito.
 * Por eso cada byte sale de `os_alloc`, que se lo pide al sistema.
 * \~
 */

#include "util/symbols/link_map.h"

#include "util/os/os_memory.h"
#include "util/report/alloc_csv_c.h"
#include "util/symbols/module_symbols.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace util {
namespace {

/**
 * @brief Donde viven los tramos y sus nombres, hasta el final del proceso.
 *
 * El asignador guarda el PUNTERO a la tabla y a cada cadena -- no copia, que es
 * su contrato --, asi que esto no se puede liberar mientras alguien pueda pedir
 * un informe.  Se pide al sistema en trozos grandes: son decenas de miles de
 * tramos y una reserva por cada uno seria justo lo contrario de lo que esto
 * intenta medir.
 */
struct Arena {
    char *text = nullptr;     ///< las cadenas, una detras de otra
    size_t text_cap = 0;
    size_t text_used = 0;
    VestaAllocCodeRange *ranges = nullptr;
    unsigned cap = 0;
    unsigned n = 0;
    bool full_said = false; ///< el aviso de que no cabe mas, UNA vez
};

Arena &arena() {
    static Arena a;
    return a;
}

/// Crece el array de tramos al doble, copiando.  Falso si el sistema dice que
/// no, y entonces se deja de anadir -- con lo leido hasta ahi, que sigue siendo
/// cierto.
bool grow_ranges(Arena &a) {
    const unsigned want = a.cap == 0 ? 4096 : a.cap * 2;
    const size_t bytes = size_t(want) * sizeof(VestaAllocCodeRange);
    void *mem = os_alloc(bytes, kOsReadWrite);
    if (mem == nullptr) return false;
    VestaAllocCodeRange *next = static_cast<VestaAllocCodeRange *>(mem);
    if (a.ranges != nullptr) {
        std::memcpy(next, a.ranges, size_t(a.n) * sizeof(VestaAllocCodeRange));
        os_free(a.ranges, size_t(a.cap) * sizeof(VestaAllocCodeRange));
    }
    a.ranges = next;
    a.cap = want;
    return true;
}

/**
 * @brief Guarda una cadena en el arena y devuelve donde quedo.
 *
 * NO SE REUBICA NUNCA.  Los punteros que se entregan al asignador apuntan aqui
 * dentro, asi que crecer copiando dejaria esos punteros mirando a memoria
 * liberada -- y el fallo no saldria al copiar, saldria al escribir el informe,
 * con nombres de otro fichero.  Por eso se encadenan trozos y el anterior se
 * queda donde esta.
 */
const char *keep(Arena &a, const char *s, size_t n) {
    if (n == 0) return nullptr;
    if (a.text == nullptr || a.text_used + n + 1 > a.text_cap) {
        const size_t want = 1u << 20;
        void *mem = os_alloc(want, kOsReadWrite);
        if (mem == nullptr) return nullptr;
        a.text = static_cast<char *>(mem);
        a.text_cap = want;
        a.text_used = 0;
    }
    if (n + 1 > a.text_cap) return nullptr; // una cadena absurda: se descarta
    char *const out = a.text + a.text_used;
    std::memcpy(out, s, n);
    out[n] = '\0';
    a.text_used += n + 1;
    return out;
}

/// El campo siguiente de la linea, saltando espacios.  Devuelve su largo.
size_t field(const char *line, size_t len, size_t &at, const char *&start) {
    while (at < len && (line[at] == ' ' || line[at] == '\t')) ++at;
    start = line + at;
    const size_t from = at;
    while (at < len && line[at] != ' ' && line[at] != '\t') ++at;
    return at - from;
}

bool starts_with(const char *s, size_t n, const char *what) {
    const size_t m = std::strlen(what);
    return n >= m && std::memcmp(s, what, m) == 0;
}

/**
 * @brief El FUENTE del que salio un objeto, y el objetivo que lo contiene.
 *
 * El enlazador escribe el objeto de dos formas, y las dos hacen falta:
 *
 *     libvx_lib.a(ir_facts.cpp.obj)          dentro de un archivo
 *     CMakeFiles/vm.dir/src/main.cpp.obj     suelto
 *
 * En la primera el objetivo es el archivo -- sin `lib` delante ni extension --;
 * en la segunda, el directorio `<objetivo>.dir`, que es como lo pone CMake y no
 * una convencion nuestra.  El fichero es el nombre del objeto sin su extension,
 * que es exactamente el fuente: asi, en Release y sin un solo simbolo, el
 * informe puede decir "esto lo reservo codigo compilado de `ir_facts.cpp`".
 */
void split_object(const char *obj, size_t n, Arena &a, const char *&file,
                  const char *&module) {
    file = nullptr;
    module = nullptr;
    const char *par = static_cast<const char *>(std::memchr(obj, '(', n));
    const char *inner = obj;
    size_t inner_n = n;
    if (par != nullptr && obj[n - 1] == ')') {
        inner = par + 1;
        inner_n = size_t(obj + n - 1 - inner);
        const char *arch = obj;
        size_t arch_n = size_t(par - obj);
        for (size_t i = arch_n; i > 0; --i)
            if (arch[i - 1] == '/' || arch[i - 1] == '\\') {
                arch += i;
                arch_n -= i;
                break;
            }
        if (starts_with(arch, arch_n, "lib")) {
            arch += 3;
            arch_n -= 3;
        }
        for (size_t i = arch_n; i > 0; --i)
            if (arch[i - 1] == '.') {
                arch_n = i - 1;
                break;
            }
        module = keep(a, arch, arch_n);
    } else {
        for (size_t i = 0; i + 5 <= n; ++i) {
            if (std::memcmp(obj + i, ".dir/", 5) != 0) continue;
            size_t start = 0;
            for (size_t k = i; k > 0; --k)
                if (obj[k - 1] == '/' || obj[k - 1] == '\\') {
                    start = k;
                    break;
                }
            module = keep(a, obj + start, i - start);
            break;
        }
        for (size_t i = inner_n; i > 0; --i)
            if (inner[i - 1] == '/' || inner[i - 1] == '\\') {
                inner += i;
                inner_n -= i;
                break;
            }
    }
    for (size_t i = inner_n; i > 0; --i)
        if (inner[i - 1] == '.') {
            inner_n = i - 1;
            break;
        }
    file = keep(a, inner, inner_n);
}

/**
 * @brief La base preferida, sacada de la CABECERA del modulo cargado.
 *
 * Es el respaldo de lo que dice el mapa, y hace falta en ELF: ahi el mapa no
 * trae `__image_base__`, y la base preferida de un ejecutable PIE es CERO --
 * por eso el sistema lo puede poner donde quiera.
 *
 * CERO ES UNA RESPUESTA, no la ausencia de una, y esa distincion es justo lo
 * que faltaba: tratando el cero como "no se sabe" no se reubicaba nada, y en
 * Linux -- donde PIE es lo normal -- el mapa se cargaba entero y no casaba ni
 * una direccion.  De ahi que la respuesta venga con su bandera.
 *
 * En PE no se puede preguntar a la imagen cargada: el cargador PARCHEA ese
 * campo al reubicar y contestaria la base actual, con lo que el desplazamiento
 * saldria cero.  Por eso ahi manda lo que diga el mapa y esto no llega a
 * usarse.
 */
unsigned long long preferred_base_of(const void *loaded, bool *found) {
    *found = false;
    const unsigned char *const p = static_cast<const unsigned char *>(loaded);
    if (p == nullptr) return 0;
#if defined(_WIN32)
    if (p[0] != 'M' || p[1] != 'Z') return 0;
    unsigned int e_lfanew = 0;
    std::memcpy(&e_lfanew, p + 0x3C, sizeof e_lfanew);
    const unsigned char *const nt = p + e_lfanew;
    if (nt[0] != 'P' || nt[1] != 'E') return 0;
    unsigned long long image_base = 0;
    std::memcpy(&image_base, nt + 4 + 20 + 24, sizeof image_base);
    *found = true;
    return image_base;
#else
    if (p[0] != 0x7F || p[1] != 'E' || p[2] != 'L' || p[3] != 'F') return 0;
    if (p[4] != 2) return 0; // solo 64 bits: en 32 la cabecera es otra
    unsigned long long e_phoff = 0;
    unsigned short e_phentsize = 0, e_phnum = 0;
    std::memcpy(&e_phoff, p + 0x20, sizeof e_phoff);
    std::memcpy(&e_phentsize, p + 0x36, sizeof e_phentsize);
    std::memcpy(&e_phnum, p + 0x38, sizeof e_phnum);
    for (unsigned i = 0; i < e_phnum; ++i) {
        const unsigned char *const ph = p + e_phoff + i * e_phentsize;
        unsigned int p_type = 0;
        std::memcpy(&p_type, ph, sizeof p_type);
        if (p_type != 1) continue; // PT_LOAD
        unsigned long long p_vaddr = 0;
        std::memcpy(&p_vaddr, ph + 0x10, sizeof p_vaddr);
        *found = true;
        return p_vaddr;
    }
    return 0;
#endif
}

} // namespace
} // namespace util

extern "C" unsigned vesta_alloc_load_link_map(void) {
    using namespace util;
    Arena &a = arena();
    if (a.n != 0) return a.n; // ya se leyo: se entrego una vez y vale

    VestaModuleInfo self;
    if (!vesta_module_of(
            reinterpret_cast<const void *>(&vesta_alloc_load_link_map), &self) ||
        self.path == nullptr)
        return 0;

    char path[1024];
    const size_t plen = std::strlen(self.path);
    if (plen + 5 >= sizeof path) return 0;
    std::memcpy(path, self.path, plen);
    std::memcpy(path + plen, ".map", 5);
    FILE *f = std::fopen(path, "rb");
    if (f == nullptr) return 0;

    /* LA BASE PREFERIDA SALE DEL MAPA, no de la cabecera del modulo cargado:
     * Windows PARCHEA ese campo al reubicar, asi que preguntarselo devuelve la
     * base actual y el desplazamiento sale cero -- el mapa se carga entero y no
     * casa ni un marco.  El enlazador de GNU escribe la buena asi, igual en PE
     * y en ELF:
     *
     *     0x0000000140000000    __image_base__ = 0x140000000
     */
    unsigned long long preferred = 0;
    bool has_preferred = false;

    char line[2048];
    size_t len = 0;
    bool pending_text = false;
    char buf[8192];
    for (;;) {
        const size_t got = std::fread(buf, 1, sizeof buf, f);
        for (size_t i = 0; i <= got; ++i) {
            const bool end = i == got;
            if (!end && buf[i] != '\n') {
                if (buf[i] != '\r' && len + 1 < sizeof line) line[len++] = buf[i];
                continue;
            }
            if (end && (got != 0 || len == 0)) break;

            size_t at = 0;
            const char *tok = nullptr;
            size_t tok_n = field(line, len, at, tok);

            if (!has_preferred && starts_with(tok, tok_n, "0x")) {
                bool has = false;
                for (size_t k = 0; k + 14 <= len; ++k)
                    if (std::memcmp(line + k, "__image_base__", 14) == 0) {
                        has = true;
                        break;
                    }
                if (has) {
                    preferred = std::strtoull(tok + 2, nullptr, 16);
                    has_preferred = true;
                }
            }

            /* DOS FORMAS, Y LA SEGUNDA ES LA DEL GRUESO.  Cuando el nombre de
             * la seccion es largo -- con C++ SIEMPRE, porque lleva dentro el
             * simbolo decorado -- el enlazador parte la fila y deja la
             * direccion en la siguiente.  Medido en un binario de verdad: 2.928
             * tramos en una linea y 19.722 en dos.  Entender solo la primera
             * carga el 13% del mapa y no casa ni un marco: un mapa "cargado"
             * que no contesta, que es el peor resultado posible. */
            if (starts_with(tok, tok_n, ".text") && at >= len) {
                pending_text = true;
                len = 0;
                if (end) break;
                continue;
            }
            const bool cont = pending_text && starts_with(tok, tok_n, "0x");
            pending_text = false;
            if (cont || starts_with(tok, tok_n, ".text")) {
                if (cont) at = 0; // la direccion ya era el primer campo
                const char *addr = nullptr, *size = nullptr, *obj = nullptr;
                const size_t addr_n = field(line, len, at, addr);
                const size_t size_n = field(line, len, at, size);
                const size_t obj_n = field(line, len, at, obj);
                if (starts_with(addr, addr_n, "0x") &&
                    starts_with(size, size_n, "0x") && obj_n != 0) {
                    const unsigned long long ad =
                        std::strtoull(addr + 2, nullptr, 16);
                    const unsigned long long sz =
                        std::strtoull(size + 2, nullptr, 16);
                    if (ad != 0 && sz != 0) {
                        if (a.n == a.cap && !grow_ranges(a)) {
                            if (!a.full_said) {
                                a.full_said = true;
                                std::fprintf(stderr,
                                             "[allocator] the linker map did "
                                             "not fit whole: %u stretches "
                                             "kept\n",
                                             a.n);
                            }
                        } else {
                            const char *file = nullptr, *module = nullptr;
                            split_object(obj, obj_n, a, file, module);
                            VestaAllocCodeRange &r = a.ranges[a.n++];
                            r.addr = reinterpret_cast<const void *>(ad);
                            r.size = size_t(sz);
                            r.name = file;
                            r.module = module;
                        }
                    }
                }
            }
            len = 0;
            if (end) break;
        }
        if (got == 0) break;
    }
    std::fclose(f);
    if (a.n == 0) return 0;

    /* Y SE REUBICAN a donde el modulo esta cargado de verdad.  Sin base
     * preferida no se toca nada: un mapa sin reubicar y uno mal reubicado se
     * parecen, pero el segundo ATRIBUYE -- da nombres de otro fichero con toda
     * la confianza del mundo. */
    const unsigned long long image =
        reinterpret_cast<unsigned long long>(self.base);
    /* SI EL MAPA NO LO DIJO, se le pregunta a la cabecera del modulo -- que es
     * el caso de ELF, donde no hay `__image_base__` y la base preferida de un
     * PIE es CERO.  Cero es una RESPUESTA: tratarlo como "no se sabe" dejaba
     * sin reubicar justo el caso normal de Linux. */
    if (!has_preferred) preferred = preferred_base_of(self.base, &has_preferred);
    if (has_preferred && image != preferred) {
        const long long delta = (long long)(image - preferred);
        for (unsigned i = 0; i < a.n; ++i) {
            const long long ad =
                (long long)reinterpret_cast<unsigned long long>(a.ranges[i].addr);
            a.ranges[i].addr =
                reinterpret_cast<const void *>((unsigned long long)(ad + delta));
        }
    }
    vesta_alloc_declare_code(a.ranges, a.n);
    return a.n;
}

/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file util/self_symbols.cpp
 * @brief Poner nombre a una direccion de este mismo binario.
 *
 * Esta mitad no tiene un solo `#if` de plataforma: leer la tabla de un PE y la
 * de un ELF vive en `self_symbols_pe.cpp` y `self_symbols_elf.cpp`, y las dos
 * ofrecen la MISMA funcion (`build_table`).  Aqui solo se construye una vez,
 * se ordena y se busca.
 */

#include "util/symbols/self_symbols.h"

#include "self_symbols_internal.h"

#include "util/alloc/host_allocator.h" // AllocScope: esto declara lo que reserva
#include "util/mem/vesta_memcpy.h" // la copia es la NUESTRA, tambien aqui dentro
#include "util/os/os_memory.h"
#include "util/symbols/self_image.h"     // la ruta del propio binario, que ya sabe dar

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace util {

using symbols_detail::build_table;
using symbols_detail::Func;
using symbols_detail::Sym;
using symbols_detail::Table;

namespace {

std::atomic<const Table *> g_table{nullptr};

/// Lee un fichero entero.  Vacio si no se puede: esto es un diagnostico, y no
/// poder darlo NO es motivo para que nada falle.
std::vector<unsigned char> slurp(const std::string &path) {
    std::vector<unsigned char> out;
    FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return out;
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    if (len > 0) {
        std::fseek(f, 0, SEEK_SET);
        out.resize(size_t(len));
        if (std::fread(out.data(), 1, out.size(), f) != out.size()) out.clear();
    }
    std::fclose(f);
    return out;
}

/// Construye la tabla la primera vez.  Nulo si no se pudo, y entonces quien
/// pregunte se entera por el valor de retorno y puede decirlo.
const Table *table() noexcept {
    const Table *t = g_table.load(std::memory_order_acquire);
    if (t != nullptr) return t;

    Table *mine = nullptr;
    try {
        /* LO QUE SE RESERVA AQUI VA DECLARADO, y no por completismo: esto corre
         * cuando alguien pide el informe de reservas, asi que sin declarar nada
         * se cuela en SU PROPIA lista.  Y no de refilon -- medido, salia el
         * PRIMERO con el 89,7% de las reservas del proceso --, tapando
         * justamente lo que se venia a mirar.  Quien mide no puede aparecer sin
         * declarar entre lo medido.
         *
         * `Instant` porque el bloque con el fichero muere al salir de aqui, y
         * `Fixed` porque se pide entero de una vez: el tamano se sabe antes de
         * pedirlo.  Es la reserva mas grande de todo el proceso -- el
         * ejecutable completo -- y por eso lleva su propio ambito, separada de
         * la tabla, que vive lo contrario. */
        const std::vector<unsigned char> bytes = [] {
            const AllocScope reading(
                AllocTag(AllocUse::Instant, AllocShape::Fixed));
            /* `self_image_path()` y no una copia local: al mudarse las dos
             * piezas a la misma libreria, resulto que cada una traia su propia
             * version de "donde esta mi ejecutable".  Dos copias de eso es
             * como se llega a que una se arregle y la otra no. */
            return slurp(self_image_path());
        }();
        if (bytes.empty()) return nullptr;

        /* La TABLA, en cambio, vive lo que el proceso -- se construye una vez y
         * se consulta hasta el final --, y se construye CRECIENDO: los nombres
         * se van pegando a una cadena unica.  Dos ejes distintos de los de
         * arriba, y por eso son dos ambitos y no uno. */
        const AllocScope building(
            AllocTag(AllocUse::Long, AllocShape::Growing));
        mine = new Table();
        /* UNA sola llamada, sin `#if`: la mitad del formato ofrece siempre la
         * misma funcion y solo una de las dos unidades compila en cada
         * sistema. */
        const bool ok = build_table(bytes, *mine);
        if (!ok) {
            delete mine;
            return nullptr;
        }
        std::sort(mine->syms.begin(), mine->syms.end(),
                  [](const Sym &a, const Sym &b) { return a.rva < b.rva; });
        /* `.pdata` ya viene ordenada -- el sistema la busca en binario para
         * desenrollar, asi que no le queda otra --, pero de `st_size` en ELF no
         * hay ninguna garantia, y la busqueda de abajo si la necesita. */
        std::sort(mine->funcs.begin(), mine->funcs.end(),
                  [](const Func &a, const Func &b) { return a.begin < b.begin; });
    } catch (...) {
        /* Leer varios megas puede fallar por memoria.  Un diagnostico que no
         * se puede dar es un diagnostico que no se da, no un fallo. */
        delete mine;
        return nullptr;
    }

    const Table *expected = nullptr;
    if (g_table.compare_exchange_strong(expected, mine,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire))
        return mine;
    delete mine; // gano otro
    return expected;
}

/**
 * @brief Lo mismo, para un modulo que NO es el nuestro.
 *
 * NO HAY NADA ESPECIFICO DE "NOSOTROS" EN LA LECTURA, y eso es lo que hace que
 * esto sean veinte lineas y no otro lector: `slurp` abre cualquier ruta y
 * `build_table` entiende un PE o un ELF venga de donde venga.  Lo unico propio
 * del binario que corre era de donde se sacaba la ruta y contra que base se
 * restaba la direccion, y las dos entran ahora por parametro.
 *
 * POR QUE HACE FALTA, teniendo ya lo que da el sistema.  Porque lo que da el
 * sistema son los simbolos EXPORTADOS -- `dladdr` mira `.dynsym`, y en PE se
 * mira la tabla de exportacion --, y una funcion interna no exporta nada.  El
 * FICHERO tiene mas: la tabla de simbolos completa si no la han quitado.
 * Quedarse en el desplazamiento teniendo el fichero delante es no leerlo.
 *
 * La cache es por RUTA y pequenya a proposito: un informe toca un punado de
 * modulos, y cada tabla cuesta leerse el fichero entero una vez.
 */
/**
 * @brief
 * \~english Whether two paths name the same file, near enough.
 * \~spanish Si dos rutas nombran el mismo fichero, con lo suficiente.
 * \~
 *
 * \~english
 * Case and separator, because the two sides come from different places: one is
 * what the loader reports for the module, the other what the process was asked
 * to run as.  On Windows they differ in case often enough, and `/` against `\`
 * whenever a path crossed a shell.  This is not a general path comparison and
 * does not pretend to be: what it protects against is reading the SAME file
 * twice, and being wrong only costs what happened before it existed.
 *
 * \~spanish
 * Mayusculas y separador, porque los dos lados vienen de sitios distintos: uno
 * es lo que el cargador dice del modulo y el otro con lo que se arranco el
 * proceso.  En Windows difieren en mayusculas bastante a menudo, y `/` contra
 * `\` en cuanto una ruta pasa por una consola.  Esto no es una comparacion
 * general de rutas ni lo pretende: de lo que protege es de leer DOS VECES el
 * mismo fichero, y equivocarse solo cuesta lo que pasaba antes de que
 * existiera.
 * \~
 */
bool same_file(const char *a, const char *b) noexcept {
    if (a == nullptr || b == nullptr) return false;
    for (; *a != '\0' && *b != '\0'; ++a, ++b) {
        char x = *a, y = *b;
        if (x == '\\') x = '/';
        if (y == '\\') y = '/';
        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
        if (x != y) return false;
    }
    return *a == '\0' && *b == '\0';
}

const Table *table_for(const char *path) noexcept {
    if (path == nullptr || *path == '\0') return nullptr;

    /* \~english OUR OWN IMAGE ALREADY HAS A TABLE, so it is not read again.
     * Nothing forbade asking for it by path -- the allocator's own report does,
     * resolving its sites through `module_symbol` -- and the answer was a
     * SECOND full table of the same file: the binary read whole a second time,
     * and every symbol name interned twice.
     *
     * Measured on a 24k-line compile with a 173 MB binary: 172,9 MB in ONE
     * allocation, plus the table built out of it.  It was the second largest
     * thing the report spent on itself, and it was a duplicate.
     *
     * \~spanish NUESTRA PROPIA IMAGEN YA TIENE TABLA, asi que no se lee otra
     * vez.  Nada impedia pedirla por ruta -- lo hace el informe del propio
     * asignador, resolviendo sus sitios por `module_symbol` -- y la respuesta
     * era una SEGUNDA tabla entera del mismo fichero: el binario leido entero
     * una vez mas, y cada nombre de simbolo internado dos veces.
     *
     * Medido sobre una compilacion de 24k lineas con un binario de 173 MB:
     * 172,9 MB en UNA reserva, mas la tabla construida a partir de ella.  Era
     * lo segundo mas grande que el informe se gastaba en si mismo, y era una
     * copia.  \~ */

    /* LA CLAVE ES UN BUFFER Y NO UN `std::string`, y no es una preferencia.
     *
     * Esta cache la lee el informe del comprobador, que corre desde la lista de
     * salida: DESPUES de que los destructores de los estaticos hayan pasado.
     * Con un `std::string` aqui, para entonces su buffer ya se devolvio, y
     * compararlo es leer un bloque que el asignador ya recogio -- en los niveles
     * normales sigue mapeado y no se nota, y en el de guarda sus paginas ya no
     * estan y el proceso muere DENTRO del informe que iba a explicar el fallo.
     *
     * Un array de caracteres no tiene destructor que pueda haber corrido.  Es la
     * misma correccion que `remember_path`, y las dos salieron del mismo sitio:
     * el nivel de guarda cazando un uso despues de liberar que llevaba ahi desde
     * siempre, en el codigo de diagnostico. */
    struct Entry {
        char path[520];
        const Table *table; ///< nulo tambien se recuerda: no se reintenta
    };
    /* Un maximo, y no una lista que crece: esto corre mientras se escribe un
     * informe, y una cache sin tope aqui es memoria que aparece justo en lo que
     * se esta midiendo. */
    constexpr unsigned kMaxModules = 16;
    static Entry cache[kMaxModules];
    static unsigned used = 0;

    for (unsigned i = 0; i < used; ++i)
        if (std::strcmp(cache[i].path, path) == 0) return cache[i].table;
    if (used >= kMaxModules) return nullptr;
    /* Una ruta que no cabe no se cachea, en vez de cachearse recortada: dos
     * modulos con el mismo prefijo largo compartirian entrada y uno contestaria
     * por el otro. */
    const size_t plen = std::strlen(path);
    if (plen + 1 > sizeof(cache[0].path)) return nullptr;

    /* \~english AFTER the cache and not before it: asking this costs a
     * `std::string` from `self_image_path()`, and the answer gets remembered in
     * the entry below -- so it is paid once per module and never on the way
     * that just reads the cache.
     * \~spanish DESPUES de la cache y no antes: preguntar esto cuesta un
     * `std::string` de `self_image_path()`, y la respuesta se recuerda en la
     * entrada de abajo -- asi que se paga una vez por modulo y nunca en el
     * camino que solo lee la cache.  \~ */
    {
        const std::string self = self_image_path();
        if (same_file(path, self.c_str())) {
            const Table *t = table();
            std::memcpy(cache[used].path, path, plen + 1);
            cache[used].table = t;
            ++used;
            return t;
        }
    }

    const Table *built = nullptr;
    Table *mine = nullptr;
    try {
        /* Declarado, por lo mismo que arriba: quien mide no puede aparecer sin
         * declarar entre lo medido. */
        const std::vector<unsigned char> bytes = [path] {
            const AllocScope reading(
                AllocTag(AllocUse::Instant, AllocShape::Fixed));
            return slurp(path);
        }();
        if (!bytes.empty()) {
            const AllocScope building(
                AllocTag(AllocUse::Long, AllocShape::Growing));
            mine = new Table();
            if (build_table(bytes, *mine)) {
                std::sort(mine->syms.begin(), mine->syms.end(),
                          [](const Sym &a, const Sym &b) { return a.rva < b.rva; });
                std::sort(mine->funcs.begin(), mine->funcs.end(),
                          [](const Func &a, const Func &b) { return a.begin < b.begin; });
                built = mine;
                mine = nullptr;
            }
        }
    } catch (...) {
        // Un diagnostico que no se puede dar no se da; no falla nada.
    }
    delete mine;

    util::vesta_memcopy(cache[used].path, path, plen + 1);
    cache[used].table = built;
    ++used;
    return built;
}

/// El simbolo que cubre @p rva dentro de @p t, o nulo.  La misma busqueda que
/// usa el camino propio, con la base ya restada por quien llama.
const char *lookup(const Table *t, uint64_t rva, size_t *offset) noexcept {
    if (t == nullptr || t->syms.empty()) return nullptr;
    const auto it = std::upper_bound(
        t->syms.begin(), t->syms.end(), rva,
        [](uint64_t v, const Sym &s) { return v < s.rva; });
    if (it == t->syms.begin()) return nullptr;
    const Sym &s = *(it - 1);
    if (offset != nullptr) *offset = size_t(rva - s.rva);
    return t->names.c_str() + s.name;
}

} // namespace

const char *module_symbol(const char *path, const void *base, const void *pc,
                          size_t *offset) noexcept {
    if (offset != nullptr) *offset = 0;
    if (pc == nullptr || base == nullptr) return nullptr;
    const uintptr_t b = uintptr_t(base);
    const uintptr_t addr = uintptr_t(pc);
    if (addr < b) return nullptr; // no es de este modulo
    return lookup(table_for(path), uint64_t(addr - b), offset);
}

const char *self_symbol(const void *pc, size_t *offset) noexcept {
    if (offset != nullptr) *offset = 0;
    const Table *t = table();
    if (t == nullptr || pc == nullptr) return nullptr;

    const uintptr_t base = uintptr_t(os_module_base());
    const uintptr_t addr = uintptr_t(pc);
    if (addr < base) return nullptr; // no es de este modulo
    const uint64_t rva = uint64_t(addr - base);

    /* El ULTIMO simbolo que empieza en o antes de la direccion.  Sin tamanos no
     * se puede saber si la direccion cae DENTRO de esa funcion o en un hueco
     * detras, asi que el desplazamiento lo dice: uno enorme significa que el
     * simbolo de al lado no es el bueno. */
    const auto it = std::upper_bound(
        t->syms.begin(), t->syms.end(), rva,
        [](uint64_t v, const Sym &s) { return v < s.rva; });
    if (it == t->syms.begin()) return nullptr;
    const Sym &s = *(it - 1);
    if (offset != nullptr) *offset = size_t(rva - s.rva);
    return t->names.c_str() + s.name;
}

bool self_function_range(const void *pc, const void **begin,
                         size_t *size) noexcept {
    if (begin != nullptr) *begin = nullptr;
    if (size != nullptr) *size = 0;
    const Table *t = table();
    if (t == nullptr || pc == nullptr || t->funcs.empty()) return false;

    const uintptr_t base = uintptr_t(os_module_base());
    const uintptr_t addr = uintptr_t(pc);
    if (addr < base) return false;
    const uint64_t rva = uint64_t(addr - base);

    const auto it = std::upper_bound(
        t->funcs.begin(), t->funcs.end(), rva,
        [](uint64_t v, const Func &f) { return v < f.begin; });
    if (it == t->funcs.begin()) return false;
    const Func &f = *(it - 1);
    /* El ultimo tramo que EMPIEZA antes puede acabar antes tambien: entre dos
     * funciones hay relleno, y una hoja sin datos de desenrollado no tiene
     * registro.  Caer ahi es "no se sabe", que es una respuesta, no un fallo. */
    if (rva >= f.end) return false;

    if (begin != nullptr) *begin = reinterpret_cast<const void *>(base + f.begin);
    if (size != nullptr) *size = size_t(f.end - f.begin);
    return true;
}

size_t self_symbol_count() noexcept {
    const Table *t = table();
    return t == nullptr ? 0 : t->syms.size();
}

size_t self_function_count() noexcept {
    const Table *t = table();
    return t == nullptr ? 0 : t->funcs.size();
}

} // namespace util

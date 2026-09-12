/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file util/alloc_csv.cpp
 * @brief The allocation report as data.  Reasons: the header.
 *
 * THE ONE RULE HERE: what goes out is what was MEASURED, with nothing rounded,
 * merged or left out.  A tool can fold and sort as it likes; it cannot invent
 * what was never written.  So the counts go raw -- including the part that is
 * an upper bound, in its own column, instead of quietly folded into the total.
 */

#include "util/report/alloc_csv.h"

#include "util/report/alloc_sites.h"
#include "util/alloc/alloc_tag.h"
#include "util/alloc/host_allocator.h"
#include "util/symbols/module_symbols.h" // si el sitio es de este modulo o de otro
#include "util/os/os_memory.h"
#include "util/alloc/size_buckets.h"
#include "util/mem/vesta_memcpy.h"
#include "util/mem/vesta_memset.h"

#include <cstdio>
#include <cstring>

/* Solo para crear la carpeta de salida.  Ver `ensure_dir`. */
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace util {

namespace {

/// Installed by the host.  Plain pointers: they are set once, at start-up.
AllocSymbolResolver g_resolver = nullptr;
AllocNameFormatter g_formatter = nullptr;
AllocModuleClassifier g_classifier = nullptr;

/**
 * @brief Lo que los modulos han DECLARADO de si mismos.
 *
 * Una tabla fija y no una lista que crece: esto se rellena desde constructores
 * estaticos, o sea DENTRO del programa que se esta midiendo y antes de que nada
 * este listo, asi que no puede reservar -- reservar aqui mete al informe en la
 * medida, que es justo lo que este proyecto lleva toda la vida separando.
 *
 * Sesenta y cuatro entradas: una por modulo, no por fichero.  En este proyecto
 * son unos veinte; cuando se acaben se DICE y se descarta el resto, en vez de
 * quedarse callada perdiendo declaraciones.
 */
constexpr unsigned kMaxDeclared = 64;
constexpr unsigned kMaxDeclaredDir = 256;

struct DeclaredModule {
    char dir[kMaxDeclaredDir]; ///< normalizado: barras normales, sin la final
    const char *name;          ///< literal de quien declara; no se copia
    unsigned len;              ///< largo de `dir`, para ganar el mas largo
};

DeclaredModule g_declared[kMaxDeclared];
unsigned g_declared_n = 0;
bool g_declared_full_said = false;

/// Copia normalizando barras y quitando la final.  Dos rutas que son la misma
/// llegan escritas de dos formas -- `__FILE__` con barras invertidas, CMake con
/// normales -- y sin esto no casarian nunca.
unsigned copy_dir(char *out, unsigned cap, const char *in) noexcept {
    unsigned n = 0;
    while (in[n] != '\0' && n + 1 < cap) {
        out[n] = in[n] == '\\' ? '/' : in[n];
        ++n;
    }
    while (n > 0 && out[n - 1] == '/') --n;
    out[n] = '\0';
    return n;
}

/**
 * @brief Writes a field, quoting it when it needs it.
 *
 * A C++ function name is FULL of commas and quotes -- one template argument
 * list has half a dozen -- so writing it raw would split it across columns and
 * the tool would read the tail of a type as if it were the next field.  The
 * rule is the usual one: wrap in quotes and double the inner ones.
 */
void csv_field(FILE *f, const char *s) {
    if (s == nullptr) return; // empty field: unknown, and it stays visible
    if (std::strpbrk(s, ",\"\n\r") == nullptr) {
        std::fputs(s, f);
        return;
    }
    std::fputc('"', f);
    for (const char *p = s; *p != '\0'; ++p) {
        if (*p == '"') std::fputc('"', f);
        std::fputc(*p, f);
    }
    std::fputc('"', f);
}

/**
 * @brief Makes @p path exist, creating every component it is missing.
 *
 * WHY THE LIBRARY DOES THIS instead of asking for a directory that is already
 * there.  Because the other way round has one failure mode and it is a bad
 * one: the run finishes, the report is gone, and the only trace is a line
 * saying a folder was missing.  Whoever asked for a report wants the report,
 * not homework -- and the directory is not a decision, it is plumbing.
 *
 * Component by component, because the useful thing to pass is a path with
 * several levels (`build/profile/alloc`), and creating only the last one would
 * fail for exactly the same reason.  A component that already exists is not an
 * error: two exports into the same place is the normal case.
 */
bool ensure_dir(const char *path) {
    if (path == nullptr || path[0] == '\0') return true; // el directorio actual
    char buf[1024];
    const size_t n = std::strlen(path);
    if (n >= sizeof(buf)) return false;
    vesta_memcpy(buf, path, n + 1);

    for (size_t i = 0; i <= n; ++i) {
        const bool end = i == n;
        if (!end && buf[i] != '/' && buf[i] != '\\') continue;
        /* Ni la raiz ni la unidad se crean: `C:` y `/` ya estan, y pedirlo
         * fallaria por algo que no es un fallo. */
        if (i == 0 || (i == 2 && buf[1] == ':')) continue;
        const char saved = buf[i];
        buf[i] = '\0';
#if defined(_WIN32)
        const bool ok = CreateDirectoryA(buf, nullptr) != 0 ||
                        GetLastError() == ERROR_ALREADY_EXISTS;
#else
        const bool ok = ::mkdir(buf, 0777) == 0 || errno == EEXIST;
#endif
        buf[i] = saved;
        if (!ok) return false;
    }
    return true;
}

/// Opens `<dir>/<name>`.  Null if it cannot, having said so.
FILE *open_in(const char *dir, const char *name) {
    char path[1024];
    const int n = std::snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (n <= 0 || size_t(n) >= sizeof(path)) {
        std::fprintf(stderr, "[allocator] CSV: path too long for %s\n", name);
        return nullptr;
    }
    FILE *f = std::fopen(path, "wb");
    if (f == nullptr)
        std::fprintf(stderr,
                     "[allocator] CSV: could not write %s -- does the "
                     "directory exist?\n",
                     path);
    return f;
}

/// How many frames are asked for per site.  Deep chains are real -- eighteen
/// were measured in one -- so this is generous; what does not fit is said in
/// `summary.csv` rather than silently cut.
constexpr unsigned kMaxFrames = 64;

/// Sites taken in one snapshot.  Same reasoning as the text report: with room
/// for only a few the list stops being "the sites" and becomes "some of them".
constexpr unsigned kMaxSites = 4096;

} // namespace

AllocSymbolResolver alloc_symbol_resolver() noexcept { return g_resolver; }

void alloc_set_symbol_resolver(AllocSymbolResolver fn) noexcept {
    g_resolver = fn;
}

void alloc_set_name_formatter(AllocNameFormatter fn) noexcept {
    g_formatter = fn;
}

void alloc_set_module_classifier(AllocModuleClassifier fn) noexcept {
    g_classifier = fn;
}

const char *alloc_root_of(const char *file_macro,
                          const char *relative) noexcept {
    if (file_macro == nullptr || relative == nullptr) return nullptr;
    /* Mismo almacen que los otros ganchos, y mismo contrato: vale hasta la
     * llamada siguiente.  Se llama una vez al arrancar, asi que 512 sobran para
     * cualquier arbol y no hace falta reservar en un camino que existe
     * precisamente para medir reservas. */
    static char held[512];

    /* SE COMPARA NORMALIZADO.  `__FILE__` puede traer barras invertidas y la
     * ruta relativa se escribe con normales; sin esto, la busqueda no encuentra
     * nada y quien llama se queda sin raiz sin enterarse. */
    size_t n = 0;
    while (file_macro[n] != '\0' && n + 1 < sizeof held) ++n;
    for (size_t i = 0; i < n; ++i)
        held[i] = file_macro[i] == '\\' ? '/' : file_macro[i];
    held[n] = '\0';

    size_t m = 0;
    while (relative[m] != '\0') ++m;
    if (m == 0 || m > n) return nullptr;

    /* La cola tiene que ser EXACTAMENTE la ruta relativa, comparada tambien con
     * las barras normalizadas de los dos lados. */
    for (size_t i = 0; i < m; ++i) {
        const char a = held[n - m + i];
        const char b = relative[i] == '\\' ? '/' : relative[i];
        if (a != b) return nullptr;
    }
    held[n - m] = '\0';
    return held;
}

void alloc_declare_module(const char *dir, const char *name) noexcept {
    if (dir == nullptr || name == nullptr || name[0] == '\0') return;
    if (g_declared_n >= kMaxDeclared) {
        /* UNA VEZ, y no por declaracion perdida: si se llena la tabla, quien la
         * llene son decenas de modulos y el mensaje saldria decenas de veces
         * tapando lo demas.  Pero se dice: una declaracion que se descarta en
         * silencio es un modulo que sale mal clasificado sin motivo aparente. */
        if (!g_declared_full_said) {
            g_declared_full_said = true;
            std::fprintf(stderr,
                         "[allocator] no room for more declared modules (%u): "
                         "the rest fall back to the classifier\n",
                         kMaxDeclared);
        }
        return;
    }
    DeclaredModule &m = g_declared[g_declared_n];
    m.len = copy_dir(m.dir, kMaxDeclaredDir, dir);
    if (m.len == 0) return;
    m.name = name;
    ++g_declared_n;
}

void alloc_declare_module_of_file(const char *file_macro,
                                  const char *name) noexcept {
    if (file_macro == nullptr) return;
    /* El DIRECTORIO del fichero, que es lo que declara un modulo: el fichero en
     * si solo se declararia a si mismo, y quien pone la linea la pone en uno
     * cualquiera de los suyos. */
    char dir[kMaxDeclaredDir];
    const unsigned n = copy_dir(dir, kMaxDeclaredDir, file_macro);
    unsigned cut = n;
    while (cut > 0 && dir[cut - 1] != '/') --cut;
    if (cut == 0) return; // un fichero sin directorio no declara nada
    dir[cut - 1] = '\0';
    alloc_declare_module(dir, name);
}

/**
 * @brief Las tablas de codigo que ha entregado el consumidor.
 *
 * Se guarda el PUNTERO, no una copia: son tablas de decenas de miles de
 * simbolos y copiarlas dentro del asignador meteria el informe en la medida que
 * esta tomando.  Ocho cabe de sobra -- una por modulo o por biblioteca -- y al
 * llenarse se dice.
 */
constexpr unsigned kMaxCodeTables = 8;

struct CodeTable {
    const VestaAllocCodeRange *ranges;
    unsigned count;
    bool sorted; ///< comprobado UNA vez al entregarla; ver `alloc_declare_code`
};

CodeTable g_code[kMaxCodeTables];
unsigned g_code_n = 0;

/**
 * @brief Los MARCADORES de fichero: una direccion y de que fuente es.
 *
 * Tabla propia y no una mas de las de arriba, por dos razones y las dos
 * importan: llegan en el orden en que corren los constructores estaticos -- o
 * sea SIN ordenar, mientras que las tablas entregadas pueden buscarse en
 * binario -- y se llenan poco a poco, asi que entregar el array a medias
 * contaria como tramos las entradas vacias.
 *
 * Cuatro mil: es una por FICHERO que se declare, no por funcion, y con la
 * cabecera que el build fuerza se declaran TODOS -- en este proyecto son unos
 * cuatrocientos cincuenta.  Al llenarse se dice, en vez de perder marcadores en
 * silencio y dejar que sus ficheros caigan en el marcador del vecino.
 */
constexpr unsigned kMaxMarkers = 4096;

VestaAllocCodeRange g_markers[kMaxMarkers];
unsigned g_markers_n = 0;
bool g_markers_full_said = false;

/// El tramo que cubre @p pc, o nulo.  Con `size` a cero, el tramo llega hasta el
/// siguiente -- que es como se expresa un MARCADOR, y por eso hace falta mirar
/// tambien al vecino de la derecha.
const VestaAllocCodeRange *code_range_of(const void *pc) noexcept {
    const VestaAllocCodeRange *best = nullptr;
    for (unsigned t = 0; t < g_code_n; ++t) {
        const CodeTable &tab = g_code[t];
        /* ORDENADA: SE BUSCA, no se recorre.  Una tabla sacada del mapa del
         * enlazador son decenas de miles de tramos y el informe pregunta una
         * vez por cada marco de cada sitio -- recorrerla entera convierte
         * escribir el informe en minutos.  Se empieza en el ultimo tramo cuya
         * direccion no pasa de `pc` y se mira hacia atras solo lo justo. */
        if (tab.sorted) {
            unsigned lo = 0, hi = tab.count;
            while (lo < hi) {
                const unsigned mid = lo + (hi - lo) / 2;
                if (tab.ranges[mid].addr <= pc)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            if (lo == 0) continue;
            const VestaAllocCodeRange &r = tab.ranges[lo - 1];
            if (r.addr == nullptr) continue;
            if (r.size != 0) {
                const unsigned char *start =
                    static_cast<const unsigned char *>(r.addr);
                if (static_cast<const unsigned char *>(pc) < start + r.size)
                    return &r;
                continue; // cae en un hueco entre dos tramos: no es de nadie
            }
            if (best == nullptr || r.addr > best->addr) best = &r;
            continue;
        }
        for (unsigned i = 0; i < tab.count; ++i) {
            const VestaAllocCodeRange &r = tab.ranges[i];
            if (r.addr == nullptr || pc < r.addr) continue;
            if (r.size != 0) {
                const unsigned char *start =
                    static_cast<const unsigned char *>(r.addr);
                if (static_cast<const unsigned char *>(pc) >= start + r.size)
                    continue;
                /* UN TRAMO CON TAMANO GANA SIEMPRE a un marcador: uno dice
                 * donde acaba y el otro lo supone.  Sin esta preferencia, una
                 * tabla que mezcle las dos cosas contestaria segun el orden. */
                return &r;
            }
            /* Marcador: vale si es el que MAS se acerca por debajo. */
            if (best == nullptr || r.addr > best->addr) best = &r;
        }
    }
    /* Y LOS MARCADORES DE FICHERO, al final: cada uno dice "esta direccion es
     * mia" y el rango sale del orden, asi que gana el mas cercano por debajo.
     * Es una aproximacion -- el enlazador reordena y puede colocar codigo de
     * otro fichero en medio -- y por eso solo contesta cuando ningun tramo con
     * TAMANO lo ha hecho ya, y por eso el informe la llama `nearby`. */
    for (unsigned i = 0; i < g_markers_n; ++i) {
        const VestaAllocCodeRange &m = g_markers[i];
        if (m.addr == nullptr || pc < m.addr) continue;
        if (best == nullptr || m.addr > best->addr) best = &m;
    }
    return best;
}

/// El modulo declarado que cubre @p file, el MAS LARGO de los que casan.  Nulo
/// si ninguno: el mas largo gana para que un subsistema dentro de otro se pueda
/// declarar sin que el de fuera se lo trague.
const char *declared_module(const char *file) noexcept {
    if (file == nullptr || g_declared_n == 0) return nullptr;
    char path[1024];
    const unsigned n = copy_dir(path, sizeof path, file);
    const char *best = nullptr;
    unsigned best_len = 0;
    for (unsigned i = 0; i < g_declared_n; ++i) {
        const DeclaredModule &m = g_declared[i];
        if (m.len > n || m.len <= best_len) continue;
        unsigned k = 0;
        while (k < m.len && path[k] == m.dir[k]) ++k;
        /* Y LA FRONTERA TIENE QUE SER UN DIRECTORIO: sin esto, `src/ir`
         * reclamaria `src/irrelevante.cpp`, que es el fallo clasico de comparar
         * prefijos de rutas como si fueran cadenas cualesquiera. */
        if (k != m.len || path[k] != '/') continue;
        best = m.name;
        best_len = m.len;
    }
    return best;
}

void alloc_declare_file(const void *pc, const char *file) noexcept {
    if (pc == nullptr || file == nullptr || file[0] == '\0') return;
    if (g_markers_n >= kMaxMarkers) {
        if (!g_markers_full_said) {
            g_markers_full_said = true;
            std::fprintf(stderr,
                         "[allocator] no room for more file markers (%u): the "
                         "rest of the files fall back to their module\n",
                         kMaxMarkers);
        }
        return;
    }
    /* SOLO EL NOMBRE, no la ruta: es lo que se ensena y lo que hace comparable
     * un informe entre dos maquinas, donde el arbol esta en sitios distintos. */
    const char *name = file;
    for (const char *p = file; *p != '\0'; ++p)
        if (*p == '/' || *p == '\\') name = p + 1;
    VestaAllocCodeRange &m = g_markers[g_markers_n++];
    m.addr = pc;
    m.size = 0; // marcador: vale hasta el siguiente.  Ver `code_range_of`
    m.name = name;
    m.module = nullptr;
}

void alloc_declare_code(const VestaAllocCodeRange *ranges,
                        unsigned count) noexcept {
    if (ranges == nullptr || count == 0) return;
    if (g_code_n >= kMaxCodeTables) {
        std::fprintf(stderr,
                     "[allocator] no room for more code tables (%u): the rest "
                     "of the addresses fall back to paths and names\n",
                     kMaxCodeTables);
        return;
    }
    /* SE COMPRUEBA EL ORDEN UNA VEZ, aqui, y no en cada consulta.  Una tabla
     * desordenada se usa igual -- se recorre entera -- pero se DICE: callarlo
     * seria dejar que una busqueda rapida conteste mal en una tabla que no
     * cumple lo que la busqueda da por hecho. */
    bool sorted = true;
    for (unsigned i = 1; i < count && sorted; ++i)
        if (ranges[i].addr < ranges[i - 1].addr) sorted = false;
    if (!sorted)
        std::fprintf(stderr,
                     "[allocator] the code table is not sorted by address: it "
                     "is used whole, which is slower but right\n");
    g_code[g_code_n].ranges = ranges;
    g_code[g_code_n].count = count;
    g_code[g_code_n].sorted = sorted;
    ++g_code_n;
}

const char *alloc_code_name(const void *pc) noexcept {
    const VestaAllocCodeRange *r = code_range_of(pc);
    return r != nullptr ? r->name : nullptr;
}

AllocModuleAnswer alloc_module_answer(const void *pc, const char *file,
                                      const char *function,
                                      const char *known) noexcept {
    /* LO QUE YA SE SABE MANDA.  Un marco con modulo salio del binario de otro,
     * y lo de aqui abajo habla del arbol del consumidor: dejarlo pisar eso
     * convertiria una DLL del sistema en un modulo del proyecto en cuanto una
     * regla casara por accidente. */
    if (known != nullptr && known[0] != '\0')
        return AllocModuleAnswer{known, "resolver"};
    /* DESPUES LO DECLARADO, que es lo que el codigo DICE de si mismo, y luego
     * la regla del consumidor.  Las dos necesitan una RUTA, asi que cuando hay
     * informacion de depuracion contestan ellas -- y contestan mas fino: dicen
     * el subsistema (`ir`, `emmit`) donde la direccion solo puede decir el
     * objetivo que se enlazo (`vmcore`).
     *
     * LA DIRECCION VA LA ULTIMA, y no porque valga menos: es EXACTA -- un
     * tramo del mapa del enlazador tiene principio y final -- pero contesta una
     * pregunta mas gruesa.  Va detras porque es lo que QUEDA cuando no hay
     * ruta, que en una construccion de distribucion es siempre: medido, 1.174
     * marcos y ninguno con fichero.  Ponerla delante costaba granularidad en la
     * unica configuracion que la tenia.
     *
     * Y SE DICE DE DONDE SALE CADA RESPUESTA.  Un rango con tamano es una
     * medida; un marcador es una aproximacion por vecindad; una regla sobre
     * rutas es una suposicion.  Las tres son utiles y NO valen lo mismo, asi
     * que la que conteste viaja junto a la respuesta en vez de quedar en la
     * cabeza de quien lo implemento. */
    if (const char *declared = declared_module(file))
        return AllocModuleAnswer{declared, "declared"};
    if (g_classifier != nullptr)
        if (const char *guessed = g_classifier(file, function))
            return AllocModuleAnswer{guessed, "rule"};
    if (const VestaAllocCodeRange *r = code_range_of(pc)) {
        if (r->module != nullptr && r->module[0] != '\0')
            return AllocModuleAnswer{r->module,
                                     r->size != 0 ? "address" : "nearby"};
        /* UN MARCADOR SIN MODULO SIGUE CONTESTANDO ALGO: el fichero.  Y la
         * columna tiene que decirlo, porque la de fichero no tiene su propia
         * marca: sin esto, un nombre sacado por vecindad -- que puede ser el
         * del fichero de al lado -- se leia exactamente igual que uno medido.
         * Medido: con dos ficheros declarados de setecientos, 1.090 marcos
         * salian con el nombre del marcador anterior y nada que lo dijera. */
        if (r->size == 0 && r->name != nullptr)
            return AllocModuleAnswer{nullptr, "nearby"};
    }
    return AllocModuleAnswer{nullptr, nullptr};
}

const char *alloc_module_of(const void *pc, const char *file,
                            const char *function, const char *known) noexcept {
    return alloc_module_answer(pc, file, function, known).name;
}

const char *alloc_module_of(const char *file, const char *function,
                            const char *known) noexcept {
    return alloc_module_answer(nullptr, file, function, known).name;
}

const char *alloc_readable_name(const char *raw) noexcept {
    if (raw == nullptr) return nullptr;
    if (g_formatter == nullptr) return raw;
    const char *pretty = g_formatter(raw);
    /* Un formateador que no sabe que hacer devuelve nulo, y entonces se queda
     * el crudo.  Que no pueda EMPEORAR la respuesta es parte del contrato:
     * quien lo instala no tiene que acordarse de devolver el original. */
    return pretty != nullptr ? pretty : raw;
}

bool write_alloc_csv(const char *dir) {
    if (dir == nullptr || dir[0] == '\0') return false;
    if (!ensure_dir(dir)) {
        std::fprintf(stderr,
                     "[allocator] CSV: could not create '%s'.  The report was "
                     "asked for and cannot be given, so this says it instead "
                     "of finishing quietly.\n",
                     dir);
        return false;
    }

    static AllocSite sites[kMaxSites];
    const unsigned n = alloc_sites_snapshot(sites, kMaxSites);
    const HostAllocStats st = host_alloc_stats();
    const unsigned long long base =
        (unsigned long long)(uintptr_t)os_module_base();

    bool ok = true;
    unsigned long long deepest = 0;
    unsigned long long unresolved = 0;

    // -- sites.csv ----------------------------------------------------------
    //
    // `allocs` is what the site EARNED and `over` what it inherited when it
    // evicted a weaker entry: the truth is in [allocs, allocs+over].  The two
    // travel apart on purpose -- folding them would hand the tool a number that
    // looks exact and is not.
    if (FILE *f = open_in(dir, "sites.csv")) {
        std::fprintf(f,
                     "site_id,allocs,bytes,over_allocs,over_bytes,large,"
                     "size_classes,class_mask,tag,tag_name,pc_offset,"
                     "foreign\n");
        for (unsigned i = 0; i < n; ++i) {
            const AllocTag tag = AllocTag::from_raw(sites[i].tag);
            unsigned classes = 0;
            for (unsigned b = 0; b < 64; ++b)
                if ((sites[i].class_mask >> b) & 1u) ++classes;
            /* La MASCARA sale ademas de la cuenta, y no es redundante: la
             * cuenta de un sitio no se puede sumar con la de otro -- dos
             * sitios que usan la misma clase darian dos --, asi que una
             * herramienta que agrupe sitios no puede decir cuantas clases
             * distintas hay en un grupo sin los bits.  Con ellos es un OR y un
             * recuento; sin ellos hay que inventarse un numero o no decir
             * nada.  Es la regla de esta cabecera: lo que sale es lo MEDIDO,
             * no un resumen del que no se puede volver. */
            std::fprintf(f, "%u,%llu,%llu,%llu,%llu,%llu,%u,%llu,%u,", i,
                         (unsigned long long)(sites[i].count - sites[i].over),
                         (unsigned long long)(sites[i].bytes -
                                              sites[i].over_bytes),
                         (unsigned long long)sites[i].over,
                         (unsigned long long)sites[i].over_bytes,
                         (unsigned long long)sites[i].large, classes,
                         (unsigned long long)sites[i].class_mask,
                         unsigned(sites[i].tag));
            csv_field(f, alloc_tag_name(tag));
            /* The address goes as an OFFSET from the module base, never
             * absolute: with address space layout randomisation the absolute
             * one means nothing in another run, and the tool would resolve it
             * against the wrong place without any way to notice. */
            std::fprintf(f, ",%llu",
                         (unsigned long long)((uintptr_t)sites[i].pc) - base);
            /* WHETHER THE SITE IS OURS AT ALL, and it travels as a FACT rather
             * than being worked out again downstream.  Since the allocator
             * serves the C runtime from the inside, a site can belong to
             * `msvcrt.dll` or to `libc.so`, and a tool asked to show "only my
             * own calls" has to be able to tell.  It cannot: the `module`
             * column holds whatever the resolver put there -- often a LOGICAL
             * module of the project -- so from the outside a system library and
             * a part of the program look the same.
             *
             * The one who knows is here, and knows for certain, so it says so.
             * The offset above is the other half: for a foreign site it is a
             * displacement from OUR base, which means nothing -- this column is
             * what stops anyone reading it as if it did. */
            std::fprintf(f, ",%d\n",
                         vesta_module_is_self(sites[i].pc) != 0 ? 0 : 1);
        }
        if (std::fclose(f) != 0) ok = false;
    } else {
        ok = false;
    }

    // -- frames.csv ---------------------------------------------------------
    //
    // Depth 0 is the innermost frame -- where the code physically is -- and the
    // last one is the function that exists in the binary.  With no resolver
    // installed the file is written with just its header: an empty table says
    // "nothing is known", a missing file says "something broke".
    if (FILE *f = open_in(dir, "frames.csv")) {
        /* Y DE DONDE SALE EL MODULO, que no es adorno: un tramo declarado con
         * tamano es una medida y una regla sobre rutas es una suposicion, y sin
         * esta columna las dos se leen igual.  Ver `AllocModuleAnswer`. */
        std::fprintf(f, "site_id,depth,inlined,function,file,line,module,"
                        "module_from\n");
        if (g_resolver != nullptr) {
            AllocFrame frames[kMaxFrames];
            for (unsigned i = 0; i < n; ++i) {
                /* Se limpia ANTES de cada llamada, y no una sola vez al
                 * declararlo.  Un resolutor solo rellena lo que sabe -- el
                 * contrato lo dice --, asi que un campo que no toque quedaria
                 * con lo que dejo el sitio ANTERIOR: un modulo equivocado que
                 * parece bueno, que es peor que uno vacio. */
                /* The TYPED C++ form, not the byte one: it gets `sizeof` and
                 * `alignof` from the type instead of being told a byte count
                 * with the alignment thrown away, which is all the plain-C
                 * entry point can do. */
                vesta_memfill(frames, 0, kMaxFrames);
                const unsigned got = g_resolver(sites[i].pc, frames, kMaxFrames);
                if (got == 0) {
                    ++unresolved;
                    /* SIN MARCOS, PERO NO SIN RESPUESTA.  Un sitio que el
                     * resolutor no sabe nombrar no escribia NINGUNA fila, asi
                     * que se quedaba sin fichero y sin modulo aunque el mapa del
                     * enlazador supiera perfectamente de que objeto es.
                     *
                     * Y no es un caso raro: medido en Linux, el resolutor no
                     * saca marcos para 991 de 1.021 sitios -- los mismos que
                     * mueven millones de reservas --, con lo que el informe se
                     * quedaba mudo justo donde tenia el dato.  Una fila con lo
                     * que SI se sabe es mas respuesta que ninguna fila. */
                    const AllocModuleAnswer mod =
                        alloc_module_answer(sites[i].pc, nullptr, nullptr,
                                            nullptr);
                    const char *const from_map = alloc_code_name(sites[i].pc);
                    if (mod.name != nullptr || from_map != nullptr) {
                        std::fprintf(f, "%u,0,0,,", i);
                        csv_field(f, from_map);
                        std::fprintf(f, ",0,");
                        csv_field(f, mod.name);
                        std::fputc(',', f);
                        csv_field(f, mod.from);
                        std::fputc('\n', f);
                    }
                }
                if (got > deepest) deepest = got;
                for (unsigned k = 0; k < got; ++k) {
                    std::fprintf(f, "%u,%u,%d,", i, k, frames[k].inlined ? 1 : 0);
                    /* Por el formateador: lo que sale es como lo escribe quien
                     * nos enlaza, no un `_ZNSt7__cxx11` que obligaria a la
                     * herramienta a traer su propio desmanglador. */
                    csv_field(f, alloc_readable_name(frames[k].function));
                    std::fputc(',', f);
                    /* Y SI NO HAY FICHERO, el que diga el tramo declarado.
                     * En Release es lo unico que queda: el resolutor no sabe
                     * nada y el mapa del enlazador si sabe de que fuente se
                     * compilo ese codigo.  Sin esto, el informe tiene el dato y
                     * no lo ensena. */
                    csv_field(f, frames[k].file != nullptr
                                     ? frames[k].file
                                     : alloc_code_name(sites[i].pc));
                    std::fprintf(f, ",%u,", frames[k].line);
                    const AllocModuleAnswer mod = alloc_module_answer(
                        sites[i].pc, frames[k].file, frames[k].function,
                        frames[k].module);
                    csv_field(f, mod.name);
                    std::fputc(',', f);
                    csv_field(f, mod.from);
                    std::fputc('\n', f);
                }
            }
        } else {
            unresolved = n;
        }
        if (std::fclose(f) != 0) ok = false;
    } else {
        ok = false;
    }

    // -- sizes.csv y tags.csv ----------------------------------------------
    if (FILE *f = open_in(dir, "sizes.csv")) {
        std::fprintf(f, "bucket,upper_bytes,allocs\n");
        for (unsigned b = 0; b < kSizeBuckets; ++b)
            std::fprintf(f, "%u,%llu,%llu\n", b, bucket_limit_out(b),
                         (unsigned long long)st.size_hist[b]);
        if (std::fclose(f) != 0) ok = false;
    } else {
        ok = false;
    }

    // -- site_sizes.csv -----------------------------------------------------
    //
    // The same split as `sizes.csv`, but PER SITE, which is the one the other
    // one cannot give: a global histogram cannot be handed back out to the
    // sites that formed it, so without this a branch of the tree can say which
    // sizes it touches (the class mask) but not how many of each -- and a site
    // with a million 32-byte allocations and one of 16 MiB looks exactly like
    // one that does the opposite.
    //
    // SPARSE, one row per (site, bucket) that is not zero.  Twelve rows per
    // site would be mostly zeros: most sites ask for one or two sizes.
    if (FILE *f = open_in(dir, "site_sizes.csv")) {
        // CUANTAS y CUANTOS BYTES, las dos: contar esconde justo lo que se
        // viene a mirar aqui, porque una casilla abarca un rango de tamanos y
        // la ultima ni siquiera tiene techo.  Un sitio con 32 reservas de mas
        // de 16 MiB es el 0,00004% de las reservas y puede ser la mitad de la
        // memoria; de la cuenta eso no sale, ni siquiera acotado.
        std::fprintf(f, "site_id,bucket,upper_bytes,allocs,bytes\n");
        for (unsigned i = 0; i < n; ++i)
            for (unsigned b = 0; b < kSizeBuckets; ++b)
                if (sites[i].size_hist[b] != 0)
                    std::fprintf(f, "%u,%u,%llu,%u,%llu\n", i, b,
                                 bucket_limit_out(b), sites[i].size_hist[b],
                                 (unsigned long long)sites[i].bytes_hist[b]);
        if (std::fclose(f) != 0) ok = false;
    } else {
        ok = false;
    }

    if (FILE *f = open_in(dir, "tags.csv")) {
        std::fprintf(f, "tag,tag_name,allocs\n");
        for (uint32_t g = 0; g < AllocTag::kSlots; ++g) {
            std::fprintf(f, "%u,", g);
            csv_field(f, alloc_tag_name(AllocTag::from_raw((uint8_t)g)));
            std::fprintf(f, ",%llu\n", (unsigned long long)st.by_tag[g]);
        }
        if (std::fclose(f) != 0) ok = false;
    } else {
        ok = false;
    }

    // -- summary.csv --------------------------------------------------------
    //
    // Key/value, so a column added later does not break a tool that reads it.
    // And it carries what could NOT be done, which is the part a tool cannot
    // work out on its own: without it, a tree with no names looks like a
    // program that allocates from nowhere.
    if (FILE *f = open_in(dir, "summary.csv")) {
        std::fprintf(f, "key,value\n");
        std::fprintf(f, "sites,%u\n", n);
        std::fprintf(f, "sites_capacity,%u\n", kMaxSites);
        std::fprintf(f, "small_allocs,%llu\n",
                     (unsigned long long)st.small_allocs);
        std::fprintf(f, "small_frees,%llu\n",
                     (unsigned long long)st.small_frees);
        std::fprintf(f, "remote_frees,%llu\n",
                     (unsigned long long)st.remote_frees);
        std::fprintf(f, "large_allocs,%llu\n",
                     (unsigned long long)st.large_allocs);
        std::fprintf(f, "large_frees,%llu\n",
                     (unsigned long long)st.large_frees);
        std::fprintf(f, "chunks,%llu\n", (unsigned long long)st.chunks);
        // THE NAME SAYS IT IS CUMULATIVE, because it is: it adds up everything
        // ever committed, and a range that gets reused counts again.  It was
        // called `bytes_reserved` and the page led with it as "committed",
        // which is a different thing -- measured: 3.328 MiB here on a process
        // that never held more than 2.374.
        std::fprintf(f, "committed_over_the_run,%llu\n",
                     (unsigned long long)st.bytes_reserved);
        // AND WHAT IT ACTUALLY NEEDED, asked of the system: this one also
        // counts the stacks, the image, and whatever a library committed on its
        // own.  It is the figure to open with, because it is the question
        // everybody arrives with -- the sites add up to what went THROUGH the
        // allocator, which was less than half of it.
        const OsProcessMemory pm = os_process_memory();
        std::fprintf(f, "process_committed_peak,%llu\n",
                     (unsigned long long)pm.commit_peak);
        std::fprintf(f, "process_resident_peak,%llu\n",
                     (unsigned long long)pm.working_set_peak);
        std::fprintf(f, "untagged,%llu\n",
                     (unsigned long long)st.by_tag[AllocTag{}.raw()]);
        std::fprintf(f, "evicted,%llu\n",
                     (unsigned long long)alloc_sites_overflow());
        std::fprintf(f, "module_base,%llu\n", base);
        std::fprintf(f, "has_symbols,%d\n", g_resolver != nullptr ? 1 : 0);
        std::fprintf(f, "sites_without_frames,%llu\n", unresolved);
        std::fprintf(f, "deepest_chain,%llu\n", deepest);
        std::fprintf(f, "frames_capacity,%u\n", kMaxFrames);
        if (std::fclose(f) != 0) ok = false;
    } else {
        ok = false;
    }

    if (!ok)
        std::fprintf(stderr,
                     "[allocator] CSV: the export is INCOMPLETE, so do not "
                     "read it: a report with holes looks like data.\n");
    return ok;
}

} // namespace util

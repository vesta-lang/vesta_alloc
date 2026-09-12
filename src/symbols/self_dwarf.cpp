/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file util/self_dwarf.cpp
 * @brief De una direccion de este binario a la CADENA de funciones inlineadas.
 *
 * Lo que queda aqui es la respuesta: la unidad ya leida y guardada, el memo de
 * lo ya resuelto, y la API publica.  Leer el formato -- el cursor, las
 * abreviaturas, las formas, los rangos, las entradas, el programa de lineas y
 * el indice de unidades -- vive en los `dwarf_*.cpp` de al lado, y lo que se
 * piden unos a otros esta escrito en `dwarf_internal.h`.
 *
 * Estaba todo en un fichero de mil setecientas lineas dentro de un namespace
 * anonimo, que es comodo de escribir y opaco de leer: nada obligaba a decir
 * que necesitaba cada parte de las demas.
 */

#include "util/symbols/self_dwarf.h"

#include "dwarf/dwarf_internal.h"

#include "util/alloc/host_allocator.h" // AllocScope: esto declara lo que reserva
#include "util/os/os_memory.h" // donde esta CARGADO el modulo
#include "util/symbols/self_image.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace util {

using namespace dwarf;

namespace {

/**
 * @brief Una unidad ya leida, con sus funciones localizadas.
 *
 * POR QUE SE GUARDA.  Porque resolver una direccion recorria la unidad ENTERA
 * desde cero, y los sitios se amontonan: en una compilacion real, 491 sitios
 * caen en unas decenas de unidades.  Medido con VTune, ese recorrido --
 * `read_die`, `read_form` y los primitivos del cursor -- son 2,0 de los 4,5
 * segundos de CPU del informe.
 *
 * Y no era solo el recorrido: `self_section_part` ABRE EL FICHERO y copia los
 * bytes, dos veces por consulta (la cabecera y luego la unidad).  Por eso el
 * perfil tenia un `RtlCopyMemory` del nucleo entre lo mas caro sin que hubiera
 * ninguna copia a la vista en este codigo.
 *
 * Se guardan los bytes, la tabla de abreviaturas y las entradas que pueden
 * contener una direccion -- funciones y funciones inlineadas -- con su
 * profundidad.  Contestar pasa a ser recorrer esa lista y preguntar si
 * contiene la direccion, sin descodificar nada.
 */
/**
 * @brief
 * \~english What is kept about one subprogram of a unit.
 * \~spanish Lo que se guarda de un subprograma de una unidad.
 * \~
 *
 * \~english
 * THE RANGES AND WHERE TO READ THE REST, not the whole entry.  This list has one
 * element per subprogram and inlined subroutine of the unit -- thousands in a
 * unit of any size -- and of all of them, only the handful that CONTAIN the
 * address being resolved ever gets read past its ranges.  Keeping the whole
 * `DieInfo` meant carrying a hundred and forty bytes of name, declaration file,
 * line and bases for every entry, to answer a question that needs twenty-four.
 *
 * Measured on a 24k-line compile: building these lists was 740 MB of the 1.317
 * the report spent on itself, the single largest consumer of the whole process
 * -- more than the program being measured.
 *
 * The entry that does match is read again from `off`, which costs one DIE per
 * frame of the answer and is paid only by the frames that are the answer.  A
 * DWARF entry is self-describing given its offset and the abbreviation table,
 * so reading it again out of order is not a special case.
 *
 * \~spanish
 * LOS RANGOS Y DONDE LEER LO DEMAS, no la entrada entera.  Esta lista tiene un
 * elemento por subprograma y subrutina inlineada de la unidad -- miles en una
 * unidad de cualquier tamano -- y de todos ellos solo del punado que CONTIENE la
 * direccion que se resuelve se llega a leer nada mas alla de sus rangos.
 * Guardar el `DieInfo` entero era arrastrar ciento cuarenta bytes de nombre,
 * fichero de declaracion, linea y bases por cada entrada, para contestar una
 * pregunta que necesita veinticuatro.
 *
 * Medido sobre una compilacion de 24k lineas: construir estas listas eran 740 MB
 * de los 1.317 que el informe se gastaba en si mismo, el mayor consumidor de
 * todo el proceso -- mas que el programa medido.
 *
 * La entrada que si coincide se vuelve a leer desde `off`, lo que cuesta un DIE
 * por marco de la respuesta y lo pagan solo los marcos que SON la respuesta.
 * Una entrada de DWARF se describe a si misma dado su desplazamiento y la tabla
 * de abreviaturas, asi que releerla fuera de orden no es un caso especial.
 * \~
 */
struct CuCandidate {
    /// \~english where the entry starts inside `CuWalk::info`.
    /// \~spanish donde empieza la entrada dentro de `CuWalk::info`.  \~
    uint32_t off;
    int32_t depth;
    /// \~english the three fields `die_contains` looks at, and no more.
    /// \~spanish los tres campos que mira `die_contains`, y ninguno mas.  \~
    uint64_t low, high, ranges;
    bool has_low, has_high, has_ranges, ranges_is_index;
};

static_assert(sizeof(CuCandidate) <= 48,
              "un candidato es la lista mas larga del resolutor: si crece, lo"
              " hace multiplicado por los miles de entradas de cada unidad");

struct CuWalk {
    uint64_t cu_off = ~uint64_t(0);
    CuCtx cu;
    std::vector<unsigned char> info;
    std::vector<Abbrev> abbrevs;
    std::vector<CuCandidate> cand;
};

/**
 * @brief Las unidades ya leidas.
 *
 * Punteros y no objetos: lo que se devuelve son punteros a la entrada, y un
 * `vector` de objetos los invalidaria al crecer.  Acotado -- pasado el tope se
 * deja de guardar y se sigue leyendo como siempre, mas lento pero nunca mal --
 * porque cada entrada se lleva los bytes de su unidad, que son decenas de KiB.
 */
/* \~english AND THEY OUTLIVE THE PROCESS ON PURPOSE, which is why they are
 * pointers that nobody deletes rather than objects.
 *
 * Who asks here is, among others, the checker's report -- and that runs from
 * the exit list, AFTER static destruction.  As plain objects their destructors
 * had already run by then, so walking the cache read freed memory: at the
 * ordinary levels it is still mapped and the wrong answer looks like a right
 * one, and at the guard level the pages are gone and the process dies inside
 * the report that was about to explain something else.  Found exactly that way,
 * with a null read in `cu_walk`.
 *
 * A cache that lives as long as the process and is read during teardown cannot
 * be destroyed at teardown.  The memory is returned when the process ends,
 * which is the only moment it stops being needed.
 *
 * It is the third time this shape has bitten -- `remember_path` and `table_for`
 * were the other two -- and all three are in the code that exists to explain
 * other people's bugs.
 *
 * \~spanish Y SOBREVIVEN AL PROCESO A PROPOSITO, que es por lo que son punteros
 * que nadie borra en vez de objetos.
 *
 * Quien pregunta aqui es, entre otros, el informe del comprobador -- y ese corre
 * desde la lista de salida, DESPUES de la destruccion de estaticos.  Como
 * objetos, para entonces sus destructores ya habian pasado, asi que recorrer la
 * cache leia memoria liberada: en los niveles normales sigue mapeada y la
 * respuesta equivocada tiene el aspecto de una buena, y en el de guarda las
 * paginas ya no estan y el proceso muere dentro del informe que iba a explicar
 * otra cosa.  Encontrado justo asi, con una lectura de nulo en `cu_walk`.
 *
 * Una cache que vive lo que el proceso y se lee mientras se desmonta no puede
 * destruirse al desmontarlo.  La memoria se devuelve cuando el proceso acaba,
 * que es el unico momento en que deja de hacer falta.
 *
 * Es la tercera vez que muerde esta forma -- `remember_path` y `table_for`
 * fueron las otras dos -- y las tres estan en el codigo que existe para explicar
 * los fallos de los demas.  \~ */
std::vector<std::unique_ptr<CuWalk>> &cu_cache() {
    static std::vector<std::unique_ptr<CuWalk>> *const v =
        new std::vector<std::unique_ptr<CuWalk>>();
    return *v;
}

std::mutex &cu_mutex() {
    static std::mutex *const m = new std::mutex();
    return *m;
}

constexpr size_t kCuCacheMax = 96;

/// Lee y recorre una unidad.  Nulo si no se pudo.
std::unique_ptr<CuWalk> build_cu_walk(const Store &st, uint64_t cu_off) {
    auto w = std::unique_ptr<CuWalk>(new CuWalk());
    w->cu_off = cu_off;

    /* La unidad, y solo la unidad: son unas decenas de KiB de los 81 MiB que
     * ocupa `.debug_info`.  Eso es lo que hace practicable preguntar. */
    uint64_t unit_len = 0;
    {
        const auto head = self_section_part(".debug_info", cu_off, 12);
        if (head.empty()) return nullptr;
        Cursor hc(head);
        const InitialLength il = read_initial_length(hc);
        if (!hc.ok()) return nullptr;
        unit_len = uint64_t(hc.p - head.data()) + il.length;
    }
    w->info = self_section_part(".debug_info", cu_off, unit_len);
    if (w->info.empty()) return nullptr;

    Cursor c(w->info);
    const InitialLength il = read_initial_length(c);
    CuCtx &cu = w->cu;
    cu.off = cu_off;
    cu.offset_size = il.offset_size;
    cu.version = c.u16();
    uint64_t abbrev_off = 0;
    if (cu.version >= 5) {
        c.u8();
        cu.addr_size = c.u8();
        abbrev_off = c.un(il.offset_size);
    } else {
        abbrev_off = c.un(il.offset_size);
        cu.addr_size = c.u8();
    }
    if (!c.ok() || !header_is_sane(cu.version, cu.addr_size)) return nullptr;

    w->abbrevs = read_abbrev(st.abbrev, abbrev_off);
    if (w->abbrevs.empty()) return nullptr;

    /* La primera entrada es la unidad, y trae las bases que las demas
     * necesitan para resolver sus indices.  Hay que leerla antes que nada. */
    DieInfo root;
    if (!read_cu_root(c, w->abbrevs, st, cu, root)) return nullptr;

    int depth = 0;
    while (c.ok() && root.children) {
        /* ANTES de leer, que es cuando el cursor esta en su principio: es por
         * donde se vuelve si esta entrada resulta contener la direccion. */
        const uint64_t die_off = uint64_t(c.p - w->info.data());
        DieInfo d;
        bool null_die = false;
        if (!read_die(c, w->abbrevs, st, cu, d, null_die)) break;
        if (null_die) {
            --depth;
            if (depth < 0) break;
            continue;
        }
        if (d.tag == DW_TAG_subprogram || d.tag == DW_TAG_inlined_subroutine) {
            CuCandidate k;
            k.off = uint32_t(die_off);
            k.depth = int32_t(depth);
            k.low = d.low;
            k.high = d.high;
            k.ranges = d.ranges;
            k.has_low = d.has_low;
            k.has_high = d.has_high;
            k.has_ranges = d.has_ranges;
            k.ranges_is_index = d.ranges_is_index;
            w->cand.push_back(k);
        }
        if (d.children) ++depth;
    }
    return w;
}

const CuWalk *cu_walk(const Store &st, uint64_t cu_off) {
    {
        std::lock_guard<std::mutex> lk(cu_mutex());
        for (const auto &w : cu_cache())
            if (w->cu_off == cu_off) return w.get();
    }
    std::unique_ptr<CuWalk> fresh = build_cu_walk(st, cu_off);
    if (fresh == nullptr) return nullptr;

    std::lock_guard<std::mutex> lk(cu_mutex());
    /* Se mira otra vez: entre soltar el cerrojo y volver a tomarlo, otro hilo
     * pudo leer la misma unidad.  Guardar las dos dejaria dos entradas con la
     * misma clave y el doble de memoria. */
    for (const auto &w : cu_cache())
        if (w->cu_off == cu_off) return w.get();
    if (cu_cache().size() >= kCuCacheMax) {
        /* Lleno: esta unidad se usa y se tira.  Se pierde el ahorro para ella
         * y no se pierde nada mas -- que es mejor que crecer sin freno en un
         * binario con dieciseis mil unidades.
         *
         * SE GUARDA EN LA CACHE, PISANDO LA ULTIMA, en vez de en una variable
         * de hilo.  Aquella tenia los dos problemas de esta funcion a la vez:
         * en Windows una variable de hilo dentro de algo que puede correr desde
         * un `malloc` reserva la primera vez que un hilo la toca -- que es la
         * recursion que este proyecto tiene prohibida --, y su destructor corre
         * al acabar el hilo, dejando colgado el puntero que se acaba de
         * devolver si alguien lo mira despues.
         *
         * Pisar la ultima entrada tiene el mismo efecto util -- la unidad vive
         * lo que dure su uso -- sin variable de hilo ni destructor que pueda
         * haber pasado ya.  Lo que se pierde es la entrada pisada, que es
         * justamente lo que se estaba tirando de todas formas. */
        cu_cache().back() = std::move(fresh);
        return cu_cache().back().get();
    }
    cu_cache().push_back(std::move(fresh));
    return cu_cache().back().get();
}

/// Resuelve de verdad, recorriendo los DIEs.  El memo lo pone su envoltorio.
unsigned resolve_frames_uncached(const Store *st, uint64_t addr, SelfFrame *out,
                                 unsigned max) {
    /* DECLARADO EN LA ENTRADA, que es lo unico que cubre a los `std::vector` y
     * `std::string` de dentro: ellos no pueden declarar nada por si mismos.
     * Recorrer los DIEs y leer el programa de lineas es trabajo de UNA
     * resolucion y muere con ella; lo poco que sobrevive lo copia el memo de
     * `Store`, que se construye bajo su propia declaracion.
     *
     * Y hace falta porque esto corre cuando alguien pide el informe de
     * reservas: sin declarar, la resolucion se cuenta a si misma en la lista
     * que esta resolviendo. */
    const AllocScope resolving(AllocUse::Instant, AllocShape::Growing);

    const uint64_t cu_off = find_cu(*st, addr);
    if (cu_off == ~uint64_t(0)) return 0;

    const CuWalk *w = cu_walk(*st, cu_off);
    if (w == nullptr) return 0;
    const CuCtx &cu = w->cu;
    const std::vector<unsigned char> &info = w->info;
    const std::vector<Abbrev> &abbrevs = w->abbrevs;

    /* Los marcos que CONTIENEN la direccion, de fuera hacia dentro.  Se guarda
     * la entrada entera porque hara falta su nombre y su sitio de llamada. */
    SmallVector<DieInfo, 16> chain;
    /// La cadena en su punto mas hondo: es la respuesta.
    SmallVector<DieInfo, 16> best;
    SmallVector<int, 32> open; // profundidad de cada marco apilado

    for (const CuCandidate &k : w->cand) {
        /* Se cierra lo que ya no puede ser antepasado.  El recorrido original
         * lo hacia al encontrar una entrada nula; aqui basta la profundidad,
         * que es la misma condicion dicha de otra forma: nada a profundidad
         * mayor o igual que la actual puede contener a la actual. */
        while (!open.empty() && open[open.size() - 1] >= k.depth) {
            open.resize(open.size() - 1);
            chain.resize(chain.size() - 1);
        }
        /* LA PREGUNTA SE HACE CON LO POCO QUE GUARDA EL CANDIDATO, y solo la
         * entrada que la contesta que si se vuelve a leer entera.  Ver
         * `CuCandidate`: de miles de entradas por unidad, las que llegan aqui
         * son a lo sumo las de la cadena de inline. */
        DieInfo probe;
        probe.low = k.low;
        probe.high = k.high;
        probe.ranges = k.ranges;
        probe.has_low = k.has_low;
        probe.has_high = k.has_high;
        probe.has_ranges = k.has_ranges;
        probe.ranges_is_index = k.ranges_is_index;
        if (die_contains(*st, cu, probe, addr)) {
            if (k.off >= info.size()) continue;
            Cursor dc(info.data() + k.off, info.size() - k.off);
            DieInfo full;
            bool null_die = false;
            if (!read_die(dc, abbrevs, *st, cu, full, null_die) || null_die)
                continue;
            chain.push_back(full);
            open.push_back(k.depth);
            /* SE GUARDA UNA COPIA AQUI, en el momento en que la cadena esta mas
             * honda.  Leerla al final da SIEMPRE vacio: el desapilado por
             * profundidad va soltando todo segun se cierran las entradas.
             * Costo encontrarlo: no fallaba, devolvia cero marcos, que se lee
             * como "aqui no habia nada inlineado". */
            best = chain;
        }
    }
    chain = best;
    if (chain.empty()) return 0;

    /* La linea del marco MAS INTERNO sale del programa de lineas; la de los
     * demas, de donde llamaron al de dentro.  Son dos preguntas distintas y
     * mezclarlas daria la linea de la declaracion, que casi nunca es donde se
     * reserva. */
    LineFiles files;
    if (cu.has_stmt_list && !st->line.empty())
        files = read_line_header(st->line, cu.stmt_list, *st, cu);

    uint64_t inner_file = 0, inner_line = 0;
    const bool have_inner =
        files.ok && line_for_addr(st->line, files, addr, inner_file, inner_line);

    const auto file_name = [&](uint64_t idx) -> const char * {
        if (!files.ok || idx >= files.files.size()) return nullptr;
        const std::string &f = files.files[size_t(idx)];
        return f.empty() ? nullptr : st->intern(f);
    };

    /* Se entregan de DENTRO hacia fuera, que es como se leen: primero donde
     * esta el codigo, al final la funcion que existe de verdad. */
    unsigned n = 0;
    for (size_t i = chain.size(); i-- > 0 && n < max;) {
        const DieInfo &d = chain[i];
        SelfFrame &f = out[n];
        const char *name = die_name(info, 0, abbrevs, *st, cu, d);
        f.function = name != nullptr ? st->intern(std::string(name)) : nullptr;
        f.inlined = d.tag == DW_TAG_inlined_subroutine;
        f.file = nullptr;
        f.line = 0;

        if (n == 0) {
            /* El de dentro: donde esta fisicamente el codigo. */
            if (have_inner) {
                f.file = file_name(inner_file);
                f.line = unsigned(inner_line);
            } else if (d.has_decl) {
                f.file = file_name(d.decl_file);
                f.line = unsigned(d.decl_line);
            }
        } else {
            /* Los de fuera: donde llamaron al que tienen dentro, que es el
             * anterior de esta lista. */
            const DieInfo &inner = chain[i + 1];
            if (inner.has_call) {
                f.file = file_name(inner.call_file);
                f.line = unsigned(inner.call_line);
            } else if (d.has_decl) {
                f.file = file_name(d.decl_file);
                f.line = unsigned(d.decl_line);
            }
        }
        ++n;
    }
    return n;
}

} // namespace

unsigned self_inline_frames(const void *pc, SelfFrame *out,
                            unsigned max) noexcept {
    if (out == nullptr || max == 0 || pc == nullptr) return 0;
    const Store *st = store();
    if (st == nullptr || !st->usable) return 0;

    const uintptr_t loaded = uintptr_t(os_module_base());
    const uintptr_t addr_rt = uintptr_t(pc);
    if (addr_rt < loaded) return 0;
    /* De direccion de EJECUCION a direccion de ENLACE, que es en la que hablan
     * estas tablas.  Sin esta conversion no se encuentra nada y parece que el
     * binario no trae informacion. */
    const uint64_t addr = uint64_t(addr_rt - loaded) + st->link_base;

    /* LO YA RESUELTO NO SE VUELVE A RESOLVER.  Ver `Store::memo`: recorrer los
     * DIEs de una unidad es lo que cuesta, y se preguntaba dos veces por cada
     * sitio -- una para el informe de texto y otra para el CSV --. */
    {
        std::lock_guard<std::mutex> lk(st->memo_mu);
        const auto it = std::lower_bound(
            st->memo.begin(), st->memo.end(), addr,
            [](const Store::Memo &m, uint64_t v) { return m.addr < v; });
        if (it != st->memo.end() && it->addr == addr) {
            const unsigned got = it->count < max ? it->count : max;
            for (unsigned i = 0; i < got; ++i)
                out[i] = st->memo_frames[it->first + i];
            return got;
        }
    }

    /* Se resuelve pidiendo SIEMPRE el maximo, no `max`: lo que se guarda tiene
     * que servir tambien al que pregunte luego con mas sitio, o el memo
     * devolveria una cadena cortada a la medida del primero que llamo. */
    SelfFrame full[Store::kMemoFrames];
    const unsigned n = resolve_frames_uncached(st, addr, full, Store::kMemoFrames);

    {
        std::lock_guard<std::mutex> lk(st->memo_mu);
        /* Se busca otra vez: entre soltar el cerrojo y volver a tomarlo, otro
         * hilo pudo resolver la misma direccion.  Insertar sin mirar dejaria
         * dos entradas con la misma clave y la busqueda binaria daria una u
         * otra segun el dia. */
        const auto it = std::lower_bound(
            st->memo.begin(), st->memo.end(), addr,
            [](const Store::Memo &m, uint64_t v) { return m.addr < v; });
        if ((it == st->memo.end() || it->addr != addr) &&
            st->memo.size() < Store::kMemoMax) {
            const Store::Memo entry{addr, unsigned(st->memo_frames.size()), n};
            for (unsigned i = 0; i < n; ++i)
                st->memo_frames.push_back(full[i]);
            st->memo.insert(it, entry);
        }
    }

    const unsigned got = n < max ? n : max;
    for (unsigned i = 0; i < got; ++i) out[i] = full[i];
    return got;
}

bool self_dwarf_covers(const void *pc) noexcept {
    const Store *st = store();
    if (st == nullptr || !st->usable || pc == nullptr) return false;
    const uintptr_t loaded = uintptr_t(os_module_base());
    if (uintptr_t(pc) < loaded) return false;
    const uint64_t addr = uint64_t(uintptr_t(pc) - loaded) + st->link_base;
    return find_cu(*st, addr) != ~uint64_t(0);
}

size_t self_dwarf_units() noexcept {
    const Store *st = store();
    return st == nullptr ? 0 : st->units;
}

size_t self_dwarf_ranges() noexcept {
    const Store *st = store();
    return st == nullptr ? 0 : st->index.size();
}

} // namespace util

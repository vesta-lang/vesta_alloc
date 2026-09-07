/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file src/dwarf_index.cpp
 * @brief Que unidad de compilacion cubre que direccion.
 *
 * Una pieza del lector de DWARF.  Lo que le pide a las demas y lo que les
 * ofrece esta escrito en `dwarf_internal.h`.
 */

#include "dwarf_internal.h"

#include "util/alloc/host_allocator.h"
#include "util/symbols/self_image.h"

#include <algorithm>

namespace util {
namespace dwarf {

// ===========================================================================
//  El indice de unidades
// ===========================================================================

/// Lee `.debug_aranges`, que es un indice YA HECHO de direccion a unidad.
void index_from_aranges(const std::vector<unsigned char> &ar, Store &st) {
    Cursor c(ar);
    while (c.ok() && c.left() > 0) {
        const unsigned char *unit = c.p;
        const InitialLength il = read_initial_length(c);
        const unsigned char *unit_end = c.p + il.length;
        const uint16_t version = c.u16();
        const uint64_t cu_off = c.un(il.offset_size);
        const uint8_t addr_size = c.u8();
        c.u8(); // segment size
        if (!c.ok() || version != 2 || addr_size == 0 || addr_size > 8) break;

        /* Los pares empiezan alineados al doble del tamano de direccion, no
         * justo detras de la cabecera.  Sin este salto se lee basura.  La
         * alineacion es respecto al principio de la SECCION, que es como la
         * escriben las herramientas de GNU. */
        const size_t head = size_t(c.p - ar.data());
        const size_t align = size_t(addr_size) * 2;
        const size_t pad = (align - (head % align)) % align;
        c.skip(pad);

        for (;;) {
            const uint64_t lo = c.un(addr_size);
            const uint64_t len = c.un(addr_size);
            if (!c.ok() || (lo == 0 && len == 0)) break;
            if (len != 0) st.index.push_back(CuRange{lo, lo + len, cu_off});
        }
        /* El salto a la unidad siguiente se compara con el PRINCIPIO de esta,
         * no con donde se acabo de dejar el cursor.  Comparandolo con el cursor
         * -- que tras leer el par de ceros cae justo en el final -- la
         * condicion se cumplia siempre y el recorrido se paraba en la primera
         * unidad: el indice acababa con UN tramo de los miles que hay, y todo
         * lo demas salia como "sin informacion de depuracion". */
        if (unit_end <= unit || unit_end > c.end) break;
        c.p = unit_end;
    }
}

/**
 * @brief El indice, recorriendo la cabecera de cada unidad.
 *
 * ESTE ES EL CAMINO BUENO, no un respaldo.  `.debug_aranges` seria un indice ya
 * hecho, pero **GCC no lo emite**: lo poco que hay en este binario viene de la
 * biblioteca de arranque, que se distribuye ya compilada con otras opciones.
 * Medido, cubria hasta `+0xb99e0` de un ejecutable de 179 MiB -- el 0,06% --,
 * asi que fiarse de el es tener un lector que contesta por el codigo de otros y
 * calla por el propio.
 *
 * Y hay que mirar los RANGOS, no solo `low_pc`/`high_pc`: con
 * `-ffunction-sections` una unidad no ocupa un tramo seguido, sino uno por
 * funcion, y su entrada raiz lo dice con una lista.  Quedarse con el par simple
 * dejaria fuera justo las construcciones de este proyecto.
 *
 * Se lee un par de KiB por unidad con la seccion ABIERTA: por el camino de una
 * llamada suelta serian dieciseis mil aperturas del propio ejecutable.
 */
void index_by_scanning(Store &st) {
    SelfSection info(".debug_info");
    if (!info.ok()) return;

    /* Lo que se lee de cada unidad.  Solo hacen falta la cabecera y la primera
     * entrada; si esa entrada no cupiera, se vuelve a leer mas grande. */
    std::vector<unsigned char> buf(4096);
    uint64_t off = 0;
    while (off + 16 < st.info_size) {
        uint64_t want = std::min<uint64_t>(buf.size(), st.info_size - off);
        if (!info.read(off, buf.data(), want)) break;

        Cursor c(buf.data(), size_t(want));
        const InitialLength il = read_initial_length(c);
        if (!c.ok() || il.length == 0) break;
        const uint64_t next = uint64_t(c.p - buf.data()) + il.length;

        CuCtx cu;
        cu.off = off;
        cu.offset_size = il.offset_size;
        cu.version = c.u16();
        uint64_t abbrev_off = 0;
        if (cu.version >= 5) {
            c.u8(); // unit_type
            cu.addr_size = c.u8();
            abbrev_off = c.un(il.offset_size);
        } else {
            abbrev_off = c.un(il.offset_size);
            cu.addr_size = c.u8();
        }
        /* Una cabecera que no se entiende NO para el barrido: se SALTA esa
         * unidad y se sigue con la siguiente.
         *
         * La longitud va delante de todo lo demas, asi que aunque la version o
         * el tamano de direccion sean raros ya se sabe donde acaba -- y por eso
         * se puede seguir --.  Pasa con las unidades de TIPOS de DWARF 5, cuya
         * cabecera lleva dos campos mas y aqui se leeria corrida; no tienen
         * codigo, asi que saltarlas no pierde nada.
         *
         * Parando, se perdia todo lo que viniera DETRAS: medido sobre el
         * compilador, 691 unidades indexadas de las mil y pico que tiene, y las
         * direcciones de las otras salian como "sin informacion de
         * depuracion". */
        if (!c.ok() || !header_is_sane(cu.version, cu.addr_size)) {
            off += next;
            continue;
        }

        const auto abbrevs = read_abbrev(st.abbrev, abbrev_off);
        DieInfo d;
        if (read_cu_root(c, abbrevs, st, cu, d) &&
            d.tag == DW_TAG_compile_unit) {
            ++st.units;
            const uint64_t at = off;
            each_range(st, cu, d, [&st, at](uint64_t lo, uint64_t hi) {
                if (hi > lo) st.index.push_back(CuRange{lo, hi, at});
            });
        }
        off += next;
    }
}

// ===========================================================================
//  Montaje
// ===========================================================================

Store *build_store() {
    /* DECLARADO, por lo mismo que en `self_symbols`: esto corre cuando alguien
     * pide el informe de reservas, asi que sin declarar nada se colaria en su
     * propia lista.  Vive lo que el proceso y se construye creciendo. */
    const AllocScope building(AllocTag(AllocUse::Long, AllocShape::Growing));

    Store *st = new (std::nothrow) Store();
    if (st == nullptr) return nullptr;

    st->link_base = self_image_link_base();
    st->abbrev = self_section(".debug_abbrev");
    if (st->abbrev.empty()) return st; // sin DWARF: `usable` se queda en false

    st->str = self_section(".debug_str");
    st->line_str = self_section(".debug_line_str");
    st->str_offsets = self_section(".debug_str_offsets");
    st->addr = self_section(".debug_addr");
    st->ranges = self_section(".debug_ranges");
    st->rnglists = self_section(".debug_rnglists");
    st->line = self_section(".debug_line");

    uint64_t info_off = 0;
    if (!self_section_range(".debug_info", &info_off, &st->info_size))
        return st;

    /* SIEMPRE se barre.  `.debug_aranges` se anade si esta, porque es gratis y
     * cubre las unidades ajenas ya compiladas, pero NO se usa como condicion:
     * en este binario cubre el 0,06%, y con "si hay indice no barras" el lector
     * se quedaba con esas 144 entradas y contestaba cero por todo lo demas.
     * Una condicion asi no falla, calla. */
    index_by_scanning(*st);
    const auto aranges = self_section(".debug_aranges");
    if (!aranges.empty()) index_from_aranges(aranges, *st);
    if (st->units == 0) st->units = st->index.size();

    std::sort(st->index.begin(), st->index.end(),
              [](const CuRange &a, const CuRange &b) { return a.lo < b.lo; });
    st->usable = !st->index.empty();
    return st;
}

const Store *store() {
    const Store *s = g_store.load(std::memory_order_acquire);
    if (s != nullptr) return s;
    Store *mine = build_store();
    if (mine == nullptr) return nullptr;
    Store *expected = nullptr;
    if (g_store.compare_exchange_strong(expected, mine,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire))
        return mine;
    delete mine; // gano otro
    return expected;
}

/// La unidad que cubre @p addr, o `UINT64_MAX`.
uint64_t find_cu(const Store &st, uint64_t addr) {
    const auto it = std::upper_bound(
        st.index.begin(), st.index.end(), addr,
        [](uint64_t v, const CuRange &r) { return v < r.lo; });
    /* Los tramos pueden solaparse y no estan agrupados por unidad, asi que se
     * mira hacia atras un poco en vez de fiarlo al primero que cae. */
    for (auto i = it; i != st.index.begin();) {
        --i;
        if (addr >= i->lo && addr < i->hi) return i->cu;
        if (i->lo + (uint64_t(1) << 32) < addr) break; // ya se fue muy lejos
    }
    return ~uint64_t(0);
}

} // namespace dwarf
} // namespace util

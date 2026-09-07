/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file src/dwarf_ranges.cpp
 * @brief Si una direccion cae dentro de una entrada.
 *
 * Una pieza del lector de DWARF.  Lo que le pide a las demas y lo que les
 * ofrece esta escrito en `dwarf_internal.h`.
 */

#include "dwarf_internal.h"

namespace util {
namespace dwarf {

/**
 * @brief Si @p addr cae en la lista de rangos de DWARF 4 (`.debug_ranges`).
 *
 * Pares (inicio, fin) RELATIVOS a una base, cerrados por un par de ceros.  Un
 * inicio con todos los bits a uno no es un rango: cambia la base para los que
 * siguen, y saltarselo desplazaria todo lo demas.
 */
bool in_ranges_v4(const Store &st, const CuCtx &cu, uint64_t off,
                  uint64_t addr) {
    if (off >= st.ranges.size()) return false;
    Cursor c(st.ranges.data() + off, st.ranges.size() - size_t(off));
    uint64_t base = cu.low_pc;
    const uint64_t all_ones =
        (cu.addr_size >= 8) ? ~uint64_t(0) : ((uint64_t(1) << (cu.addr_size * 8)) - 1);
    for (;;) {
        const uint64_t a = c.un(cu.addr_size);
        const uint64_t b = c.un(cu.addr_size);
        if (!c.ok()) return false;
        if (a == 0 && b == 0) return false; // fin de la lista
        if (a == all_ones) {
            base = b;
            continue;
        }
        if (addr >= base + a && addr < base + b) return true;
    }
}

/// Lo mismo en DWARF 5 (`.debug_rnglists`), que codifica cada entrada con un
/// byte de tipo delante en vez de repetir siempre dos direcciones enteras.
bool in_ranges_v5(const Store &st, const CuCtx &cu, uint64_t off,
                  uint64_t addr) {
    if (off >= st.rnglists.size()) return false;
    Cursor c(st.rnglists.data() + off, st.rnglists.size() - size_t(off));
    uint64_t base = cu.low_pc;
    for (;;) {
        const uint8_t kind = c.u8();
        if (!c.ok()) return false;
        switch (kind) {
            case DW_RLE_end_of_list: return false;
            case DW_RLE_base_addressx:
                base = addr_by_index(st, cu, c.uleb());
                break;
            case DW_RLE_startx_endx: {
                const uint64_t a = addr_by_index(st, cu, c.uleb());
                const uint64_t b = addr_by_index(st, cu, c.uleb());
                if (addr >= a && addr < b) return true;
                break;
            }
            case DW_RLE_startx_length: {
                const uint64_t a = addr_by_index(st, cu, c.uleb());
                const uint64_t n = c.uleb();
                if (addr >= a && addr < a + n) return true;
                break;
            }
            case DW_RLE_offset_pair: {
                const uint64_t a = c.uleb();
                const uint64_t b = c.uleb();
                if (addr >= base + a && addr < base + b) return true;
                break;
            }
            case DW_RLE_base_address: base = c.un(cu.addr_size); break;
            case DW_RLE_start_end: {
                const uint64_t a = c.un(cu.addr_size);
                const uint64_t b = c.un(cu.addr_size);
                if (addr >= a && addr < b) return true;
                break;
            }
            case DW_RLE_start_length: {
                const uint64_t a = c.un(cu.addr_size);
                const uint64_t n = c.uleb();
                if (addr >= a && addr < a + n) return true;
                break;
            }
            /* Un codigo que no se conoce hace lo mismo que una forma que no se
             * conoce: no se puede saltar, asi que se para. */
            default: return false;
        }
        if (!c.ok()) return false;
    }
}

// ===========================================================================
//  Las DIE
// ===========================================================================

/**
 * @brief Donde empieza de verdad la lista de rangos de una entrada.
 *
 * En DWARF 4 el atributo ya es el desplazamiento.  En DWARF 5 puede serlo
 * tambien (`sec_offset`) o ser un INDICE en una tabla que hay al principio de
 * la seccion, y entonces hay que ir a buscarlo a `rnglists_base`.  Las dos
 * formas caben en el mismo atributo y solo la forma las distingue.
 */
bool ranges_offset(const Store &st, const CuCtx &cu, const DieInfo &d,
                   uint64_t &out) {
    if (!d.ranges_is_index) {
        out = d.ranges;
        return true;
    }
    const uint64_t at = cu.rnglists_base + d.ranges * cu.offset_size;
    if (at + cu.offset_size > st.rnglists.size()) return false;
    /* Tamanos constantes, por lo mismo que en `str_by_index`: el formato solo
     * admite estos dos anchos, y decirlo asi ademas evita que el compilador
     * tenga que dar por alcanzable el camino de dieciseis bytes de la copia. */
    const unsigned char *p = st.rnglists.data() + at;
    uint64_t v = 0;
    if (cu.offset_size == 8) {
        vesta_memcpy(&v, p, 8);
    } else if (cu.offset_size == 4) {
        uint32_t v32 = 0;
        vesta_memcpy(&v32, p, 4);
        v = v32;
    } else {
        return false;
    }
    /* El indice cuenta desde la BASE, asi que el desplazamiento que guarda es
     * relativo a ella y no al principio de la seccion. */
    out = cu.rnglists_base + v;
    return true;
}

/// Si @p addr cae dentro de la entrada, por rango simple o por lista.
bool die_contains(const Store &st, const CuCtx &cu, const DieInfo &d,
                  uint64_t addr) {
    if (d.has_low && d.has_high && d.high > d.low)
        return addr >= d.low && addr < d.high;
    if (d.has_ranges) {
        uint64_t off = 0;
        if (!ranges_offset(st, cu, d, off)) return false;
        return cu.version >= 5 ? in_ranges_v5(st, cu, off, addr)
                               : in_ranges_v4(st, cu, off, addr);
    }
    return false;
}

/// Recorre los rangos de una entrada llamando a @p fn con cada par.  Es lo
/// mismo que mira @c die_contains, pero enumerando en vez de preguntar: lo usa

} // namespace dwarf
} // namespace util

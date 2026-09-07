/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file src/dwarf_types.h
 * @brief Las formas que se pasan entre las piezas del lector.
 *
 * NO ES API: no se instala y nadie de fuera debe incluirlo.  Aqui estan los
 * tipos y nada mas -- el cursor acotado, una abreviatura, el contexto de una
 * unidad, una entrada --, de modo que cada `.cpp` del lector incluya esto y no
 * a los demas.
 */
#ifndef VESTA_SRC_DWARF_TYPES_H
#define VESTA_SRC_DWARF_TYPES_H

#include "dwarf_spec.h"

#include "util/alloc/small_vector.h"
#include "util/mem/vesta_memcpy.h" // el cursor lee enteros con la nuestra

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace util {
namespace dwarf {

struct Cursor {
    const unsigned char *p = nullptr;
    const unsigned char *end = nullptr;
    bool bad = false;

    Cursor() = default;
    Cursor(const unsigned char *b, size_t n) : p(b), end(b + n) {}
    explicit Cursor(const std::vector<unsigned char> &v)
        : p(v.data()), end(v.data() + v.size()) {}

    size_t left() const { return size_t(end - p); }
    bool ok() const { return !bad; }

    bool need(size_t n) {
        if (bad || left() < n) {
            bad = true;
            return false;
        }
        return true;
    }

    uint8_t u8() {
        if (!need(1)) return 0;
        return *p++;
    }
    uint16_t u16() {
        if (!need(2)) return 0;
        uint16_t v = 0;
        vesta_memcpy(&v, p, 2);
        p += 2;
        return v;
    }
    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t v = 0;
        vesta_memcpy(&v, p, 4);
        p += 4;
        return v;
    }
    uint64_t u64() {
        if (!need(8)) return 0;
        uint64_t v = 0;
        vesta_memcpy(&v, p, 8);
        p += 8;
        return v;
    }
    /// Un entero sin signo de @p n bytes, para las formas `strx1..4` y demas.
    uint64_t un(unsigned n) {
        uint64_t v = 0;
        for (unsigned i = 0; i < n; ++i) v |= uint64_t(u8()) << (8 * i);
        return v;
    }
    uint64_t uleb() {
        uint64_t v = 0;
        unsigned shift = 0;
        for (;;) {
            const uint8_t b = u8();
            if (bad) return 0;
            if (shift < 64) v |= uint64_t(b & 0x7F) << shift;
            shift += 7;
            if ((b & 0x80) == 0) break;
        }
        return v;
    }
    int64_t sleb() {
        int64_t v = 0;
        unsigned shift = 0;
        uint8_t b = 0;
        do {
            b = u8();
            if (bad) return 0;
            if (shift < 64) v |= int64_t(uint64_t(b & 0x7F) << shift);
            shift += 7;
        } while ((b & 0x80) != 0);
        if (shift < 64 && (b & 0x40) != 0) v |= -(int64_t(1) << shift);
        return v;
    }
    /// Una cadena terminada en cero DENTRO del tramo.  Nulo si no lo esta.
    const char *cstr() {
        const unsigned char *s = p;
        while (p < end && *p != 0) ++p;
        if (p >= end) {
            bad = true;
            return nullptr;
        }
        ++p;
        return reinterpret_cast<const char *>(s);
    }
    void skip(size_t n) {
        if (need(n)) p += n;
    }
};

/// La longitud inicial, que ademas dice si la unidad usa desplazamientos de
/// cuatro u ocho bytes.  Va delante de casi todo en DWARF.
struct InitialLength {
    uint64_t length = 0;
    unsigned offset_size = 4;
};

inline InitialLength read_initial_length(Cursor &c) {
    InitialLength r;
    const uint32_t first = c.u32();
    if (first == 0xFFFFFFFFu) {
        r.length = c.u64();
        r.offset_size = 8;
    } else {
        r.length = first;
        r.offset_size = 4;
    }
    return r;
}

// ===========================================================================
//  Abreviaturas
// ===========================================================================

struct AttrSpec {
    uint16_t at = 0;
    uint16_t form = 0;
    int64_t implicit = 0; ///< solo con `DW_FORM_implicit_const`
};

struct Abbrev {
    uint16_t tag = 0;
    bool children = false;
    bool used = false;
    /* Ocho caben sin pedir memoria, y la inmensa mayoria tiene menos: un
     * `subprogram` con nombre, fichero, linea y rango va sobrado. */
    SmallVector<AttrSpec, 8> attrs;
};

/**
 * @brief Las abreviaturas de una unidad, en un array PLANO indexado por codigo.
 *
 * Y no en un mapa: los codigos que emite GCC son 1, 2, 3... consecutivos desde
 * uno, asi que un array indexado es una lectura sin hash y sin sondeo -- que es
 * la regla del proyecto para las tablas de despacho --.  El tope existe por si
 * algun productor los reparte a lo loco: mas alla se ignora, y entonces esa DIE
 * no se puede leer y el recorrido para, en vez de reservar por un numero que
 * venia en el fichero.
 */
constexpr uint64_t kMaxAbbrevCode = 1u << 16;


inline bool header_is_sane(uint16_t version, unsigned addr_size) {
    if (version < 2 || version > 5) return false;
    return addr_size == 4 || addr_size == 8;
}

/// Un tramo de direcciones y la unidad que lo cubre.
struct CuRange {
    uint64_t lo;
    uint64_t hi;
    uint64_t cu; ///< desplazamiento dentro de `.debug_info`
};


struct CuCtx {
    uint64_t off = 0; ///< donde empieza la unidad en `.debug_info`
    uint16_t version = 4;
    unsigned offset_size = 4;
    unsigned addr_size = 8;
    uint64_t str_offsets_base = 0;
    uint64_t addr_base = 0;
    uint64_t rnglists_base = 0;
    uint64_t low_pc = 0;
    uint64_t stmt_list = 0;
    bool has_stmt_list = false;
    const char *comp_dir = nullptr;
};

// ===========================================================================
//  Valores
// ===========================================================================

struct Value {
    uint64_t num = 0;
    int64_t snum = 0;
    const char *str = nullptr;
};



/// Lo que interesa de una entrada, ya leido.
struct DieInfo {
    uint16_t tag = 0;
    bool children = false;
    bool has_low = false, has_high = false, has_ranges = false;
    uint64_t low = 0, high = 0, ranges = 0;
    /* En DWARF 5 `DW_AT_ranges` puede ser un INDICE en la tabla de listas en
     * vez de un desplazamiento a ella.  Tratar uno como el otro no falla:
     * apunta a otro sitio y devuelve rangos de otra funcion. */
    bool ranges_is_index = false;
    uint64_t abstract_origin = 0, specification = 0;
    bool has_origin = false, has_spec = false;
    const char *name = nullptr;
    const char *linkage = nullptr;
    uint64_t call_file = 0, call_line = 0;
    uint64_t decl_file = 0, decl_line = 0;
    bool has_call = false, has_decl = false;
    uint64_t stmt_list = 0;
    bool has_stmt_list = false;
    const char *comp_dir = nullptr;
    uint64_t str_offsets_base = 0, addr_base = 0, rnglists_base = 0;
};

/**
 * @brief Lee una entrada entera y se queda con lo que hace falta.
 * @return false si la entrada no se pudo leer -- y entonces el recorrido se
 *         para, porque a partir de ahi ya no se sabe donde empieza la siguiente.
 */

//  La tabla de ficheros y el programa de lineas
// ===========================================================================

/// Los ficheros de una unidad, ya montados con su directorio delante.
struct LineFiles {
    std::vector<std::string> files;
    bool ok = false;
    /* Donde acaba la cabecera, que es donde empieza el programa. */
    uint64_t program_off = 0;
    uint64_t end_off = 0;
    uint8_t min_inst = 1, max_ops = 1, opcode_base = 13;
    int8_t line_base = -5;
    uint8_t line_range = 14;
    bool default_is_stmt = true;
    SmallVector<uint8_t, 16> std_lens;
};

} // namespace dwarf
} // namespace util

#endif // VESTA_SRC_DWARF_TYPES_H

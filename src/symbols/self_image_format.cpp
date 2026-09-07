/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */


/**
 * @file src/self_image_format.cpp
 * @brief La mitad que SI depende del formato: PE por un lado, ELF por otro.
 *
 * Aqui esta todo lo que cambia entre un ejecutable de Windows y uno de Linux
 * -- encontrar una seccion y averiguar la base de enlace --, y nada mas.  La
 * otra mitad (`self_image.cpp`) no tiene ni un `#if` de plataforma, que es
 * justo lo que se gana partiendolo: un formato nuevo se anade aqui.
 */

#include "self_image_internal.h"

#include "util/symbols/self_image.h"

#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace util {
namespace image_detail {

FILE *open_self() {
    const std::string path = self_image_path();
    if (path.empty()) return nullptr;
    return std::fopen(path.c_str(), "rb");
}

std::vector<unsigned char> read_at(FILE *f, uint64_t off, uint64_t bytes) {
    std::vector<unsigned char> out;
    if (f == nullptr || bytes == 0) return out;
    /* Un tope de cordura: un tamano absurdo en una cabecera corrupta no debe
     * convertirse en una reserva de varios gigas.  Cuatro GiB pasa de sobra la
     * seccion mas grande que este proyecto genera (`.debug_info`, 81 MiB). */
    if (bytes > (uint64_t(4) << 30)) return out;
    if (std::fseek(f, long(off), SEEK_SET) != 0) return out;
    out.resize(size_t(bytes));
    if (std::fread(out.data(), 1, out.size(), f) != out.size()) out.clear();
    return out;
}

#if defined(_WIN32)

/**
 * @brief Busca una seccion del PE y devuelve donde esta EN EL FICHERO.
 *
 * El nombre de una seccion son ocho bytes, y `.debug_aranges` mide catorce.
 * Para eso PE tiene una segunda forma: `/` seguido del desplazamiento DECIMAL
 * dentro de la tabla de cadenas de COFF, que es donde vive el nombre entero.
 * Sin tratar ese caso no se encontraria NINGUNA seccion de depuracion, porque
 * todas pasan de ocho caracteres -- y no daria un error, daria "no hay
 * informacion de depuracion", que es la respuesta equivocada mas creible.
 */
bool find_section(FILE *f, const char *name, uint64_t *off, uint64_t *size) {
    uint16_t mz = 0;
    if (!peek(f, 0, mz) || mz != 0x5A4D) return false;
    int32_t e_lfanew = 0;
    if (!peek(f, 0x3C, e_lfanew) || e_lfanew <= 0) return false;
    uint32_t sig = 0;
    if (!peek(f, uint64_t(e_lfanew), sig) || sig != 0x00004550) return false;

    const uint64_t coff = uint64_t(e_lfanew) + 4;
    uint16_t n_sections = 0, opt_size = 0;
    uint32_t sym_ptr = 0, sym_count = 0;
    if (!peek(f, coff + 2, n_sections) || !peek(f, coff + 8, sym_ptr) ||
        !peek(f, coff + 12, sym_count) || !peek(f, coff + 16, opt_size))
        return false;

    /* La tabla de cadenas va justo detras de la de simbolos.  Si no hay
     * simbolos no hay tabla, y entonces solo se pueden reconocer los nombres
     * cortos -- que es exactamente lo que pasa en una construccion despojada, y
     * ahi tampoco hay secciones de depuracion que buscar. */
    const uint64_t strings = uint64_t(sym_ptr) + uint64_t(sym_count) * 18;

    const uint64_t sec_base = coff + 20 + opt_size;
    for (uint16_t i = 0; i < n_sections; ++i) {
        const uint64_t sh = sec_base + uint64_t(i) * 40;
        char raw[9] = {0};
        if (std::fseek(f, long(sh), SEEK_SET) != 0) return false;
        if (std::fread(raw, 1, 8, f) != 8) return false;

        bool hit = false;
        if (raw[0] == '/' && sym_ptr != 0) {
            /* Nombre largo: `/` y el desplazamiento en decimal.  Los ocho bytes
             * pueden no llevar terminador, de ahi la copia acotada de arriba. */
            const uint64_t at = strings + uint64_t(std::strtoul(raw + 1, nullptr, 10));
            char full[256] = {0};
            if (std::fseek(f, long(at), SEEK_SET) == 0) {
                const size_t got = std::fread(full, 1, sizeof(full) - 1, f);
                full[got] = '\0';
                hit = std::strcmp(full, name) == 0;
            }
        } else {
            hit = std::strncmp(raw, name, 8) == 0 && std::strlen(name) <= 8;
        }
        if (!hit) continue;

        uint32_t vsize = 0, raw_size = 0, raw_ptr = 0;
        if (!peek(f, sh + 8, vsize) || !peek(f, sh + 16, raw_size) ||
            !peek(f, sh + 20, raw_ptr))
            return false;
        /* Lo grabado se redondea hacia arriba; el tamano UTIL es el virtual
         * cuando es menor.  Leer el relleno como contenido daria basura que
         * parece dato. */
        *off = raw_ptr;
        *size = (vsize != 0 && vsize < raw_size) ? vsize : raw_size;
        return *size != 0;
    }
    return false;
}

#else

/// Lo mismo en ELF.  Aqui los nombres viven en su propia tabla de cadenas, la
/// que senala `e_shstrndx`, y no hay caso corto ni caso largo.
bool find_section(FILE *f, const char *name, uint64_t *off, uint64_t *size) {
    unsigned char ident[16];
    if (std::fseek(f, 0, SEEK_SET) != 0) return false;
    if (std::fread(ident, 1, sizeof(ident), f) != sizeof(ident)) return false;
    if (ident[0] != 0x7F || ident[1] != 'E' || ident[2] != 'L' ||
        ident[3] != 'F' || ident[4] != 2 /* 64 bits */)
        return false;

    uint64_t sh_off = 0;
    uint16_t sh_ent = 0, sh_num = 0, sh_strndx = 0;
    if (!peek(f, 0x28, sh_off) || !peek(f, 0x3A, sh_ent) ||
        !peek(f, 0x3C, sh_num) || !peek(f, 0x3E, sh_strndx))
        return false;
    if (sh_strndx >= sh_num) return false;

    /* Donde estan los NOMBRES: es una seccion mas, y hay que localizarla antes
     * de poder preguntar por ninguna otra. */
    uint64_t str_off = 0, str_size = 0;
    if (!peek(f, sh_off + uint64_t(sh_strndx) * sh_ent + 0x18, str_off) ||
        !peek(f, sh_off + uint64_t(sh_strndx) * sh_ent + 0x20, str_size))
        return false;

    const size_t want = std::strlen(name);
    for (uint16_t i = 0; i < sh_num; ++i) {
        const uint64_t sh = sh_off + uint64_t(i) * sh_ent;
        uint32_t sh_name = 0;
        if (!peek(f, sh, sh_name)) return false;
        if (uint64_t(sh_name) + want + 1 > str_size) continue;

        char buf[256] = {0};
        if (std::fseek(f, long(str_off + sh_name), SEEK_SET) != 0) continue;
        const size_t got = std::fread(buf, 1, sizeof(buf) - 1, f);
        buf[got] = '\0';
        if (std::strcmp(buf, name) != 0) continue;

        uint32_t type = 0;
        uint64_t s_off = 0, s_size = 0;
        if (!peek(f, sh + 4, type) || !peek(f, sh + 0x18, s_off) ||
            !peek(f, sh + 0x20, s_size))
            return false;
        if (type == 8 /* SHT_NOBITS */) return false; // no ocupa en el fichero
        *off = s_off;
        *size = s_size;
        return s_size != 0;
    }
    return false;
}

#endif

} // namespace image_detail

/* La base de ENLACE tambien depende del formato, asi que vive aqui aunque sea
 * publica: en PE sale de `ImageBase` de la cabecera opcional y en ELF de la
 * direccion virtual del primer tramo cargable.  No se parecen en nada. */
using image_detail::open_self;
using image_detail::peek;

uint64_t self_image_link_base() {
    FILE *f = open_self();
    if (f == nullptr) return 0;
    uint64_t base = 0;
#if defined(_WIN32)
    int32_t e_lfanew = 0;
    uint16_t magic = 0;
    if (peek(f, 0x3C, e_lfanew) && e_lfanew > 0) {
        /* La cabecera opcional empieza tras la firma (4) y la de COFF (20).
         * `ImageBase` esta en su desplazamiento 24 y mide ocho bytes en 64 bits
         * y cuatro en 32; el `Magic` del principio dice cual de los dos. */
        const uint64_t opt = uint64_t(e_lfanew) + 4 + 20;
        if (peek(f, opt, magic)) {
            if (magic == 0x20B) { // PE32+
                peek(f, opt + 24, base);
            } else if (magic == 0x10B) { // PE32
                uint32_t b32 = 0;
                if (peek(f, opt + 28, b32)) base = b32;
            }
        }
    }
#else
    /* En ELF la base de enlace es la direccion virtual del primer tramo
     * cargable.  Vale cero en un ejecutable independiente de posicion, que es
     * lo normal hoy, y ahi la resta de la cabecera ya sale bien sola. */
    uint16_t ph_num = 0, ph_ent = 0;
    uint64_t ph_off = 0;
    if (peek(f, 0x20, ph_off) && peek(f, 0x36, ph_ent) && peek(f, 0x38, ph_num)) {
        bool first = true;
        for (uint16_t i = 0; i < ph_num; ++i) {
            const uint64_t ph = ph_off + uint64_t(i) * ph_ent;
            uint32_t type = 0;
            uint64_t vaddr = 0;
            if (!peek(f, ph, type) || !peek(f, ph + 0x10, vaddr)) break;
            if (type != 1 /* PT_LOAD */) continue;
            if (first || vaddr < base) {
                base = vaddr;
                first = false;
            }
        }
    }
#endif
    std::fclose(f);
    return base;
}
} // namespace util

/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file src/self_symbols_pe.cpp
 * @brief La tabla de simbolos de un ejecutable de Windows.
 *
 * Y `.pdata` ademas, que es lo que dice donde ACABA cada funcion: la tabla de
 * simbolos solo dice donde empieza.  Fuera de Windows este fichero no compila
 * nada -- el `#if` envuelve la unidad entera --, que es como se anade un
 * formato nuevo sin tocar ninguno de los que ya estan.
 */

#include "self_symbols_internal.h"

#if defined(_WIN32)

#include <cstring>
#include <string>
#include <vector>

namespace util {
namespace symbols_detail {
namespace {


/**
 * @brief Lee la tabla de simbolos COFF de un PE.
 *
 * Un simbolo mide 18 bytes -- y NO se puede usar `sizeof` de una estructura,
 * porque el compilador la alinearia a 20 y todos los indices saldrian
 * corridos --.  El nombre son 8 bytes: si los cuatro primeros son cero, los
 * cuatro siguientes son un desplazamiento dentro de la tabla de cadenas; si no,
 * el nombre esta ahi mismo y puede no llevar terminador.
 */
/**
 * @brief Los limites EXACTOS de cada funcion, leidos de `.pdata`.
 *
 * QUE ES `.pdata`.  La tabla que usa Windows para DESENROLLAR la pila: un
 * registro por funcion con `[inicio, fin)` y donde estan sus datos de
 * desenrollado.  La necesita el sistema para propagar una excepcion, asi que en
 * x86-64 la tiene que llevar toda funcion que no sea hoja.
 *
 * Y DE AHI SALE LO BUENO: **no es una tabla de simbolos, es una seccion**, asi
 * que `--strip-all` NO se la lleva.  Medido sobre este proyecto, la version de
 * Release -- que se queda literalmente sin un solo simbolo -- conserva las
 * 34.883 entradas enteras.  Es lo unico que permite decir algo sensato de una
 * direccion en un binario despojado.
 *
 * Solo x86-64: en x86-32 no existe la seccion, y en arm64 los registros tienen
 * otra forma.  Buscarla por nombre ya hace que en x86-32 simplemente no
 * aparezca, pero la comprobacion de maquina esta puesta igual, porque
 * confundirse de formato no daria un error sino tramos inventados.
 */
bool build_pdata(const std::vector<unsigned char> &b, size_t sec_base,
                 uint16_t n_sections, uint16_t machine, Table &t) {
    if (machine != 0x8664) return false; // IMAGE_FILE_MACHINE_AMD64
    for (uint16_t i = 0; i < n_sections; ++i) {
        const size_t sh = sec_base + size_t(i) * 40;
        if (sh + 40 > b.size()) return false;
        /* El nombre de una seccion son ocho bytes SIN terminador obligatorio,
         * asi que se compara acotado y no con `strcmp`. */
        if (std::memcmp(b.data() + sh, ".pdata\0\0", 8) != 0) continue;

        uint32_t vsize = 0, raw_size = 0, raw_ptr = 0;
        if (!read_at(b, sh + 8, vsize) || !read_at(b, sh + 16, raw_size) ||
            !read_at(b, sh + 20, raw_ptr))
            return false;
        /* Lo grabado se redondea hacia arriba, asi que el tamano REAL es el
         * virtual cuando es menor: leer el relleno daria funciones fantasma. */
        const size_t bytes =
            (vsize != 0 && vsize < raw_size) ? size_t(vsize) : size_t(raw_size);

        /// `RUNTIME_FUNCTION`: inicio, fin y donde estan los datos de
        /// desenrollado.  Los tres son desplazamientos de cuatro bytes.
        constexpr size_t kEntry = 12;
        t.funcs.reserve(bytes / kEntry);
        for (size_t off = raw_ptr; off + kEntry <= size_t(raw_ptr) + bytes;
             off += kEntry) {
            uint32_t begin = 0, end = 0;
            if (!read_at(b, off, begin) || !read_at(b, off + 4, end)) break;
            if (begin == 0 || end <= begin) continue; // relleno o basura
            t.funcs.push_back(Func{begin, end});
        }
        return !t.funcs.empty();
    }
    return false;
}

bool build_pe(const std::vector<unsigned char> &b, Table &t) {
    uint16_t mz = 0;
    if (!read_at(b, 0, mz) || mz != 0x5A4D) return false; // 'MZ'
    int32_t e_lfanew = 0;
    if (!read_at(b, 0x3C, e_lfanew) || e_lfanew <= 0) return false;
    uint32_t sig = 0;
    if (!read_at(b, size_t(e_lfanew), sig) || sig != 0x00004550) return false;

    const size_t coff = size_t(e_lfanew) + 4;
    uint16_t machine = 0, n_sections = 0, opt_size = 0;
    uint32_t sym_ptr = 0, sym_count = 0;
    if (!read_at(b, coff + 0, machine) || !read_at(b, coff + 2, n_sections) ||
        !read_at(b, coff + 8, sym_ptr) || !read_at(b, coff + 12, sym_count) ||
        !read_at(b, coff + 16, opt_size))
        return false;

    /* Las secciones, para convertir (seccion, valor) en desplazamiento: el
     * valor de un simbolo es relativo al principio de SU seccion. */
    const size_t sec_base = coff + 20 + opt_size;

    /* Los tramos ANTES que los simbolos, y a proposito: es lo unico que queda
     * en un binario despojado, asi que si se leyera despues del `return` de
     * abajo no se leeria nunca justo donde mas falta hace. */
    build_pdata(b, sec_base, n_sections, machine, t);

    if (sym_ptr == 0 || sym_count == 0)
        return !t.funcs.empty(); // despojado: quedan los tramos, que ya es algo

    std::vector<uint32_t> sec_rva(n_sections, 0);
    for (uint16_t i = 0; i < n_sections; ++i)
        if (!read_at(b, sec_base + size_t(i) * 40 + 12, sec_rva[i]))
            return false;

    constexpr size_t kSymSize = 18;
    const size_t strings = size_t(sym_ptr) + size_t(sym_count) * kSymSize;

    t.names.reserve(size_t(sym_count) * 24);
    t.names.push_back('\0'); // el indice 0 significa "sin nombre"
    t.syms.reserve(sym_count);

    for (uint32_t i = 0; i < sym_count; ++i) {
        const size_t off = size_t(sym_ptr) + size_t(i) * kSymSize;
        if (off + kSymSize > b.size()) break;
        uint32_t value = 0, name_zero = 0, name_off = 0;
        int16_t section = 0;
        uint8_t storage = 0, aux = 0;
        read_at(b, off + 0, name_zero);
        read_at(b, off + 4, name_off);
        read_at(b, off + 8, value);
        read_at(b, off + 12, section);
        read_at(b, off + 16, storage);
        read_at(b, off + 17, aux);
        /* Los registros auxiliares van DETRAS del simbolo y ocupan su mismo
         * tamano; hay que saltarlos o se leerian como simbolos. */
        i += aux;

        if (section <= 0 || size_t(section) > sec_rva.size()) continue;
        /* 2 = externo, 3 = estatico.  Los demas son etiquetas, ficheros y
         * cosas que no son codigo con nombre. */
        if (storage != 2 && storage != 3) continue;

        /* El nombre se APUNTA donde ya esta, no se copia a una cadena aparte.
         *
         * Antes habia aqui un `std::string` por simbolo, y un nombre manglado
         * de C++ no cabe en los quince caracteres que una cadena guarda sin
         * pedir memoria: eran ciento treinta mil reservas que morian en la
         * misma vuelta.  Lo encontro este mismo informe -- salia el primero,
         * con el 89,7% de las reservas del proceso --, que es exactamente para
         * lo que sirve.  El nombre ya esta contiguo en `b`, asi que copiarlo a
         * un sitio para copiarlo enseguida al definitivo no compraba nada. */
        const char *np = nullptr;
        size_t nlen = 0;
        if (name_zero == 0) {
            if (name_off < b.size() - strings) {
                np = reinterpret_cast<const char *>(b.data() + strings +
                                                    name_off);
                nlen = ::strnlen(np, b.size() - (strings + name_off));
            }
        } else {
            np = reinterpret_cast<const char *>(b.data() + off);
            nlen = ::strnlen(np, 8);
        }
        /* El orden importa: sin nombre `np` es nulo, y preguntar por su primer
         * caracter antes de descartarlo lo leeria. */
        if (nlen == 0 || np[0] == '.') continue; // secciones, no funciones

        Sym s;
        s.rva = uint64_t(sec_rva[size_t(section) - 1]) + value;
        s.name = uint32_t(t.names.size());
        t.names.append(np, nlen);
        t.names.push_back('\0');
        t.syms.push_back(s);
    }
    return !t.syms.empty() || !t.funcs.empty();
}


} // namespace

bool build_table(const std::vector<unsigned char> &bytes, Table &t) {
    return build_pe(bytes, t);
}

} // namespace symbols_detail
} // namespace util

#endif // _WIN32

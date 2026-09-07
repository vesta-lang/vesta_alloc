/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file src/self_symbols_elf.cpp
 * @brief La tabla de simbolos de un ejecutable de Linux.
 *
 * Aqui el tramo de cada funcion sale de `st_size`, que ELF si trae -- en PE
 * hay que ir a buscarlo a `.pdata` --.  En Windows este fichero no compila
 * nada: el `#if` envuelve la unidad entera.
 */

#include "self_symbols_internal.h"

#if !defined(_WIN32)

#include <cstring>
#include <string>
#include <vector>

namespace util {
namespace symbols_detail {
namespace {


/// Lee `.symtab` de un ELF64.  Misma idea que el PE, otra disposicion.
bool build_elf(const std::vector<unsigned char> &b, Table &t) {
    if (b.size() < 64 || b[0] != 0x7F || b[1] != 'E' || b[2] != 'L' ||
        b[3] != 'F' || b[4] != 2 /* 64 bits */)
        return false;

    uint64_t sh_off = 0;
    uint16_t sh_ent = 0, sh_num = 0, sh_strndx = 0;
    if (!read_at(b, 0x28, sh_off) || !read_at(b, 0x3A, sh_ent) ||
        !read_at(b, 0x3C, sh_num) || !read_at(b, 0x3E, sh_strndx))
        return false;

    size_t symtab = 0, symsz = 0, strtab = 0, strsz = 0;
    for (uint16_t i = 0; i < sh_num; ++i) {
        const size_t sh = size_t(sh_off) + size_t(i) * sh_ent;
        uint32_t type = 0;
        uint64_t off = 0, size = 0, link = 0;
        uint32_t link32 = 0;
        if (!read_at(b, sh + 4, type) || !read_at(b, sh + 0x18, off) ||
            !read_at(b, sh + 0x20, size) || !read_at(b, sh + 0x28, link32))
            return false;
        link = link32;
        if (type != 2 /* SHT_SYMTAB */) continue;
        symtab = size_t(off);
        symsz = size_t(size);
        if (link < sh_num) {
            const size_t lsh = size_t(sh_off) + size_t(link) * sh_ent;
            uint64_t soff = 0, ssz = 0;
            read_at(b, lsh + 0x18, soff);
            read_at(b, lsh + 0x20, ssz);
            strtab = size_t(soff);
            strsz = size_t(ssz);
        }
        break;
    }
    (void)sh_strndx;
    if (symtab == 0 || symsz == 0 || strtab == 0) return false;

    constexpr size_t kSymSize = 24; // Elf64_Sym
    t.names.reserve(symsz);
    t.names.push_back('\0');
    for (size_t off = symtab; off + kSymSize <= symtab + symsz; off += kSymSize) {
        uint32_t st_name = 0;
        uint8_t st_info = 0;
        uint16_t st_shndx = 0;
        uint64_t st_value = 0, st_size = 0;
        if (!read_at(b, off + 0, st_name) || !read_at(b, off + 4, st_info) ||
            !read_at(b, off + 6, st_shndx) || !read_at(b, off + 8, st_value) ||
            !read_at(b, off + 16, st_size))
            break;
        if (st_shndx == 0 || st_value == 0) continue;
        if ((st_info & 0xF) != 2 /* STT_FUNC */) continue;
        if (st_name == 0 || strtab + st_name >= b.size()) continue;

        /* Igual que en la rama del PE: el nombre se apunta donde ya esta.  Una
         * cadena por simbolo eran ciento treinta mil reservas que morian en la
         * misma vuelta, porque un nombre manglado de C++ no cabe en los quince
         * caracteres que una cadena guarda sin pedir memoria. */
        const char *p =
            reinterpret_cast<const char *>(b.data() + strtab + st_name);
        const size_t max = std::min(strsz - st_name, b.size() - strtab - st_name);
        const size_t nlen = ::strnlen(p, max);
        if (nlen == 0) continue;

        Sym s;
        s.rva = st_value;
        s.name = uint32_t(t.names.size());
        t.names.append(p, nlen);
        t.names.push_back('\0');
        t.syms.push_back(s);

        /* Aqui NO hace falta el equivalente de `.pdata`: un simbolo de ELF
         * LLEVA su tamano (`st_size`), que es justo el dato que al PE le falta y
         * hay que ir a buscar a la tabla de desenrollado.  Cero significa que
         * quien lo emitio no lo dijo -- pasa con el ensamblador escrito a mano
         * --, y entonces no se apunta ningun tramo en vez de inventarse uno. */
        if (st_size != 0)
            t.funcs.push_back(
                Func{uint32_t(st_value), uint32_t(st_value + st_size)});
    }
    return !t.syms.empty() || !t.funcs.empty();
}


} // namespace

bool build_table(const std::vector<unsigned char> &bytes, Table &t) {
    return build_elf(bytes, t);
}

} // namespace symbols_detail
} // namespace util

#endif // !_WIN32

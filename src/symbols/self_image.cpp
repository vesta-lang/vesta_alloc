/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */


/**
 * @file util/self_image.cpp
 * @brief Encontrar secciones dentro del propio binario.  Motivos: la cabecera.
 *
 * Esta mitad NO tiene un solo `#if` de plataforma: todo lo que cambia entre PE
 * y ELF vive en `self_image_format.cpp`.  Anadir un formato nuevo es tocar
 * aquel fichero y ninguno mas.
 */

#include "util/symbols/self_image.h"

#include "self_image_internal.h"

#include "util/alloc/host_allocator.h" // AllocScope: esto declara lo que reserva

#include <cstdio>
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

using image_detail::find_section;
using image_detail::open_self;
using image_detail::read_at;

std::string self_image_path() {
    /* La cadena se devuelve y el llamante la suelta enseguida; el sitio que la
     * pide corre dentro del informe de reservas, asi que sin declararla se
     * cuenta a si misma.  Ver `AllocScope`. */
    const AllocScope path(AllocUse::Instant, AllocShape::Growing);
#if defined(_WIN32)
    char buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameA(nullptr, buf, sizeof(buf));
    return (n > 0 && n < sizeof(buf)) ? std::string(buf, n) : std::string();
#else
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    return n > 0 ? std::string(buf, size_t(n)) : std::string();
#endif
}

SelfSection::SelfSection(const char *name) {
    if (name == nullptr) return;
    FILE *f = open_self();
    if (f == nullptr) return;
    uint64_t off = 0, size = 0;
    if (!find_section(f, name, &off, &size)) {
        std::fclose(f);
        return;
    }
    file_ = f;
    base_ = off;
    size_ = size;
}

SelfSection::~SelfSection() {
    if (file_ != nullptr) std::fclose(static_cast<FILE *>(file_));
}

bool SelfSection::read(uint64_t offset, void *dst, uint64_t bytes) const {
    if (file_ == nullptr || dst == nullptr || bytes == 0) return false;
    /* Pedir mas alla del final NO se recorta: se falla.  Un tramo corto se
     * leeria como una estructura truncada, y eso da cosas que parecen validas. */
    if (offset > size_ || bytes > size_ - offset) return false;
    FILE *f = static_cast<FILE *>(file_);
    if (std::fseek(f, long(base_ + offset), SEEK_SET) != 0) return false;
    return std::fread(dst, 1, size_t(bytes), f) == size_t(bytes);
}

bool self_section_range(const char *name, uint64_t *offset, uint64_t *bytes) {
    if (name == nullptr) return false;
    FILE *f = open_self();
    if (f == nullptr) return false;
    uint64_t o = 0, s = 0;
    const bool ok = find_section(f, name, &o, &s);
    std::fclose(f);
    if (!ok) return false;
    if (offset != nullptr) *offset = o;
    if (bytes != nullptr) *bytes = s;
    return true;
}

std::vector<unsigned char> self_section(const char *name) {
    std::vector<unsigned char> out;
    if (name == nullptr) return out;
    FILE *f = open_self();
    if (f == nullptr) return out;
    uint64_t off = 0, size = 0;
    if (find_section(f, name, &off, &size)) out = read_at(f, off, size);
    std::fclose(f);
    return out;
}

std::vector<unsigned char> self_section_part(const char *name, uint64_t offset,
                                             uint64_t bytes) {
    std::vector<unsigned char> out;
    if (name == nullptr) return out;
    FILE *f = open_self();
    if (f == nullptr) return out;
    uint64_t off = 0, size = 0;
    if (find_section(f, name, &off, &size)) {
        /* Pedir mas alla del final NO se recorta en silencio: se devuelve
         * vacio.  Un tramo corto se leeria como una unidad de compilacion
         * truncada, y eso da estructuras que parecen validas. */
        if (offset <= size && bytes <= size - offset)
            out = read_at(f, off + offset, bytes);
    }
    std::fclose(f);
    return out;
}

} // namespace util

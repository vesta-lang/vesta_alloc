/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file tests/test_module_symbols.cpp
 * @brief That an address from ANOTHER module never comes back wearing one of
 *        our names.
 *
 * THE BUG THIS EXISTS FOR.  Every resolver in this library reads the program's
 * own image, and none of them checked that the address was the program's.  So
 * an address inside `libc` found nothing in our symbol table and came back as
 * the last symbol we happened to have -- `_fini` -- with exactly the same
 * confidence as a true name.  It showed up in a real report the day the
 * allocator started serving the C library from the inside, where every
 * allocation's address belongs to somebody else by construction.
 *
 * A report is read as fact.  A missing name asks a question; a wrong one closes
 * it.  So the check below is not "does it resolve", it is "does it refuse to
 * resolve what is not ours" -- and that is why the last one asserts a NEGATIVE.
 */

#include "util/report/alloc_csv_c.h"
#include "util/symbols/module_symbols.h"
#include "util/os/os_memory.h"
#include "util/symbols/self_image.h"
#include "util/symbols/self_resolver.h"
#include "util/symbols/self_symbols.h"

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cstdlib>
#endif

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
    std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
    if (!ok) ++g_failures;
}

/// Una funcion de ESTE modulo, para preguntar por su direccion.
[[gnu::noinline]] void from_this_module() { std::printf("%s", ""); }

/**
 * @brief Una direccion que seguro es de OTRO modulo.
 *
 * En Linux, una entrada de la libreria de C; en Windows, una de kernel32, que
 * es una DLL siempre y no se puede enlazar estaticamente.  Se coge la direccion
 * de la funcion, no se llama: lo que se prueba es a quien pertenece.
 */
const void *foreign_address() {
#if defined(_WIN32)
    return reinterpret_cast<const void *>(&GetProcAddress);
#else
    return reinterpret_cast<const void *>(&::strdup);
#endif
}

void an_address_of_ours_is_recognised_as_ours() {
    const void *mine = reinterpret_cast<const void *>(&from_this_module);
    check(vesta_module_is_self(mine) == 1,
          "una direccion de este modulo se reconoce como propia");

    VestaModuleInfo m;
    std::memset(&m, 0, sizeof(m));
    check(vesta_module_of(mine, &m) == 1 && m.path != nullptr,
          "y se sabe de que fichero es");
    /* El desplazamiento, no la direccion: es lo unico que se repite entre
     * ejecuciones, porque la direccion se mueve con cada carga. */
    check(m.offset != 0 && m.offset < (size_t(1) << 32),
          "y viene como desplazamiento dentro del modulo, no como direccion");
}

void an_address_from_elsewhere_is_not_ours() {
    const void *theirs = foreign_address();
    check(vesta_module_is_self(theirs) == 0,
          "una direccion de otro modulo NO se toma por propia");

    VestaModuleInfo m;
    std::memset(&m, 0, sizeof(m));
    check(vesta_module_of(theirs, &m) == 1 && m.path != nullptr,
          "y aun asi se sabe de que fichero es");
    check(m.symbol != nullptr,
          "y, siendo una entrada exportada, tambien como se llama alli");
}

/**
 * @brief LA REGRESION: el resolutor no le pone nuestros nombres a lo ajeno.
 *
 * Lo que se comprueba es que el nombre que sale, si sale, NO es un simbolo de
 * nuestra tabla.  `_fini` era el que salia -- el ultimo que teniamos --, asi
 * que se nombra explicitamente: si vuelve, este test lo dice con el nombre del
 * culpable en vez de con un fallo generico.
 */
void the_resolver_does_not_borrow_our_names() {
    VestaAllocFrame f[8];
    std::memset(f, 0, sizeof(f));
    const unsigned n = vesta_self_resolver(foreign_address(), f, 8);

    check(n > 0, "una direccion ajena se resuelve a ALGO, no a nada");
    if (n == 0) return;

    const char *fn = f[0].function != nullptr ? f[0].function : "";
    check(std::strcmp(fn, "_fini") != 0 && std::strcmp(fn, "_init") != 0,
          "y ese algo NO es `_fini` ni `_init`: no se le presta un nombre "
          "nuestro a una direccion que no es nuestra");
    check(f[0].module != nullptr,
          "sale ademas de que modulo es, que es la mitad util del dato");
    check(f[0].file == nullptr && f[0].line == 0,
          "y sin fichero ni linea, que de un modulo ajeno no se saben -- vacio "
          "se lee como 'no se', que es la verdad");
}

/**
 * @brief Que leer los simbolos de un modulo CUALQUIERA sea leer los de siempre.
 *
 * `module_symbol` no es un lector nuevo: es el mismo que lee el binario propio,
 * con la ruta y la base por parametro.  La forma de comprobarlo sin depender de
 * que haya un modulo ajeno con simbolos a mano es aplicarselo a NOSOTROS y
 * exigir que diga exactamente lo mismo que la version de toda la vida.
 *
 * Si un dia divergen, es que hay dos lectores otra vez.
 */
void reading_another_module_is_reading_the_same_way() {
    const void *mine = reinterpret_cast<const void *>(&from_this_module);

    size_t off_self = 0;
    const char *by_self = util::self_symbol(mine, &off_self);
    if (by_self == nullptr) {
        check(true, "(sin tabla de simbolos en esta construccion: nada que "
                    "contrastar, y eso no es un fallo)");
        return;
    }

    size_t off_mod = 0;
    const char *by_module = util::module_symbol(
        util::self_image_path().c_str(), util::os_module_base(), mine,
        &off_mod);

    check(by_module != nullptr,
          "leer el modulo por su RUTA da un nombre, igual que leerse a uno "
          "mismo");
    check(by_module != nullptr && std::strcmp(by_self, by_module) == 0,
          "y es EL MISMO nombre: un solo lector, no dos");
    check(off_self == off_mod, "y el mismo desplazamiento");
}

} // namespace

int main() {
    std::printf("== una direccion ajena no lleva nombres nuestros ==\n");
    an_address_of_ours_is_recognised_as_ours();
    an_address_from_elsewhere_is_not_ours();
    the_resolver_does_not_borrow_our_names();
    reading_another_module_is_reading_the_same_way();

    if (g_failures == 0) {
        std::printf("TODO OK\n");
        return 0;
    }
    std::printf("%d COMPROBACIONES FALLIDAS\n", g_failures);
    return 1;
}

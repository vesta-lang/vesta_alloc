/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file util/self_resolver.cpp
 * @brief El cable entre leerse a uno mismo y el gancho de la exportacion.
 *
 * Los motivos, en la cabecera.  Aqui solo esta el orden de los tres intentos y
 * lo que se hace cuando ninguno da nada.
 */

#include "util/symbols/self_resolver.h"

#include "util/symbols/module_symbols.h"
#include "util/os/os_memory.h"
#include "util/symbols/self_dwarf.h"
#include "util/symbols/self_symbols.h"

#include <cstdio>

namespace {

/// Cuantos marcos se piden a la informacion de depuracion de una vez.
constexpr unsigned kMaxFrames = 64;

/**
 * @brief Donde se escriben los "nombres" del tercer intento.
 *
 * VARIOS y no uno: quien pregunta lee los punteros DESPUES de que esto vuelva
 * -- resuelve la cadena de un sitio y luego escribe su fila --, asi que con un
 * solo buffer dos sitios seguidos sin nombre acabarian ensenando los dos el
 * ultimo texto.  Con una rueda de dieciseis, la cadena mas larga que se ha
 * medido (diecinueve marcos) sigue cabiendo holgada en el uso real, donde este
 * caso da UN marco por sitio.
 */
constexpr unsigned kFallbackSlots = 16;
char g_fallback[kFallbackSlots][64];
unsigned g_next_slot = 0;

/// La siguiente ranura de la rueda.  La comparten los dos caminos que escriben
/// texto propio, y por eso esta aqui y no dentro de uno de ellos.
char *next_slot() {
    char *buf = g_fallback[g_next_slot];
    g_next_slot = (g_next_slot + 1) % kFallbackSlots;
    return buf;
}

/**
 * @brief Un marco para una direccion que NO es de nuestra imagen.
 *
 * Lo que se puede decir de ella son dos cosas, y las dos son ciertas: de que
 * modulo es, y como se llama el simbolo mas cercano dentro de el.  Lo que NO se
 * puede es fichero y linea -- eso necesita la informacion de depuracion de un
 * fichero ajeno, que en una maquina normal ni siquiera esta instalada para la
 * libreria de C --, asi que sale vacio, que se lee como "no se".
 *
 * El desplazamiento va en el nombre a proposito cuando no hay simbolo: es lo
 * unico estable entre ejecuciones -- la direccion se mueve con cada carga --,
 * asi que dos informes del mismo programa se pueden comparar.
 */
unsigned resolve_foreign(const void *pc, VestaAllocFrame *out, unsigned max) {
    VestaModuleInfo m;
    if (max == 0 || !vesta_module_of(pc, &m)) return 0;

    out[0].file = nullptr;
    out[0].line = 0;
    out[0].inlined = 0;
    out[0].module = m.path;

    if (m.symbol != nullptr) {
        out[0].function = m.symbol;
        return 1;
    }
    /* Sin simbolo -- un modulo despojado --, pero el desplazamiento sigue
     * siendo un dato.  Se escribe con la misma forma que el tercer intento del
     * camino propio para que quien lea el informe vea UNA convencion. */
    char *buf = next_slot();
    std::snprintf(buf, 64, "+0x%llx", (unsigned long long)m.offset);
    out[0].function = buf;
    return 1;
}

} // namespace

extern "C" unsigned vesta_self_resolver(const void *pc, VestaAllocFrame *out,
                                        unsigned max) {
    if (out == nullptr || max == 0 || pc == nullptr) return 0;

    /* 0. DE QUIEN ES ESTA DIRECCION.  Antes de los tres intentos, porque los
     *    tres leen la imagen PROPIA y ninguno comprobaba que la direccion lo
     *    fuera.
     *
     *    Lo que costaba: una direccion de `libc.so` no encontraba nada en
     *    nuestra tabla de simbolos y salia con el ULTIMO simbolo que hubiera --
     *    `_fini` --, con la misma cara que un nombre cierto.  Un informe se lee
     *    como un hecho, y un nombre equivocado es peor que ninguno: el que
     *    falta hace una pregunta, el equivocado la cierra.
     *
     *    Y desde que el asignador sirve a la libreria de C por dentro esto dejo
     *    de ser un caso raro: las reservas de `strdup` y compania son de otro
     *    modulo por construccion. */
    if (!vesta_module_is_self(pc)) return resolve_foreign(pc, out, max);

    /* 1. LA INFORMACION DE DEPURACION.  Es la unica que da la cadena entera,
     *    y por eso se intenta primero. */
    util::SelfFrame frames[kMaxFrames];
    const unsigned n =
        util::self_inline_frames(pc, frames, max < kMaxFrames ? max
                                                              : kMaxFrames);
    if (n > 0) {
        const unsigned got = n < max ? n : max;
        for (unsigned i = 0; i < got; ++i) {
            out[i].function = frames[i].function;
            out[i].file = frames[i].file;
            out[i].line = frames[i].line;
            out[i].inlined = frames[i].inlined ? 1 : 0;
            /* De que MODULO es esto no lo sabe una libreria: depende de como
             * este organizado cada proyecto.  Vacio, que se lee como "no se". */
            out[i].module = nullptr;
        }
        return got;
    }

    /* 2. LA TABLA DE SIMBOLOS.  Un marco y sin fichero.  Es menos, pero es lo
     *    que hay, y devolver cero dejaria sin nombre algo que si lo tiene. */
    size_t off = 0;
    if (const char *name = util::self_symbol(pc, &off)) {
        out[0].function = name;
        out[0].file = nullptr;
        out[0].line = 0;
        out[0].inlined = 0;
        out[0].module = nullptr;
        return 1;
    }

    /* 3. Y SIN NOMBRES TAMPOCO SE DEVUELVE NADA VACIO.
     *
     * En una construccion despojada no hay simbolos ni informacion de
     * depuracion, pero `.pdata` sigue ahi: es una SECCION, no una tabla de
     * simbolos, asi que `--strip-all` no se la lleva.  De ella sale donde
     * empieza la funcion que contiene la direccion, y con eso quien lo lea
     * agrupa todos los sitios de una misma funcion en vez de dejar cada uno
     * suelto.
     *
     * No es un nombre, y no se disfraza de uno: sale como el desplazamiento
     * que es.  Pero un `fn +0x10c1760` con seis sitios debajo dice mucho mas
     * que seis direcciones sin relacion aparente. */
    const void *fn = nullptr;
    if (util::self_function_range(pc, &fn, nullptr)) {
        char *slot = next_slot();
        std::snprintf(slot, sizeof(g_fallback[0]), "fn +0x%llx",
                      (unsigned long long)((uintptr_t)fn -
                                           (uintptr_t)util::os_module_base()));
        out[0].function = slot;
        out[0].file = nullptr;
        out[0].line = 0;
        out[0].inlined = 0;
        out[0].module = nullptr;
        return 1;
    }
    return 0;
}

/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file examples/symbol_report.cpp
 * @brief Un informe de reservas CON NOMBRES, en C++.
 *
 *     VESTA_HOST_ALLOC_SITES=1 vesta_alloc_example_symbol_report [carpeta]
 *
 * QUE DEMUESTRA.  Que esta libreria, ella sola, contesta de donde viene cada
 * reserva: se lee a si misma -- secciones, tabla de simbolos e informacion de
 * depuracion --, saca la cadena de funciones inlineadas de cada direccion y la
 * exporta.  No hace falta un compilador al lado, ni `addr2line`, ni un
 * servidor de simbolos.
 *
 * Y todo el enganche son DOS lineas:
 *
 *     vesta_alloc_set_symbol_resolver(vesta_self_resolver);
 *     vesta_alloc_write_csv(dir);
 *
 * Hay un gemelo de este fichero en C puro (`c_symbol_report.c`) que hace
 * exactamente lo mismo.  Estan los dos a proposito: el mecanismo no es una
 * comodidad de C++, y la unica forma de que eso siga siendo cierto es que
 * ambos se compilen.
 *
 * POR QUE HACE FALTA LA VARIABLE DE ENTORNO.  Porque apuntar de donde viene
 * cada reserva cuesta -- medido, duplica el coste de `new`+`delete` --, y una
 * medida que nadie pidio no se paga.  Sin ella el programa corre igual y este
 * ejemplo lo DICE en vez de escribir un informe vacio.
 */

#include "util/report/alloc_csv.h"
#include "util/alloc/host_allocator.h"
#include "util/symbols/self_dwarf.h" // solo para la comprobacion del final
#include "util/symbols/self_resolver.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

/* Los punteros tienen que ESCAPAR o el compilador borra el par: C++14 le deja
 * eliminar un `new`/`delete` cuyo resultado nadie lee, y lo hace.  Esa trampa
 * ya se comio un bucle de calentamiento entero en este proyecto. */
void *volatile g_sink;

/// Reserva desde varios sitios distintos, para que el informe tenga que
/// distinguirlos.  Cada funcion es un sitio: esa es la unidad que se apunta.
void parse_tokens(std::size_t n) {
    const util::AllocScope scope(
        util::AllocTag(util::AllocUse::Instant, util::AllocShape::Fixed));
    for (std::size_t i = 0; i < n; ++i) {
        void *p = ::operator new(96);
        g_sink = p;
        ::operator delete(p);
    }
}

void build_index(std::size_t n) {
    /* `Long/Growing` porque un vector que crece ABANDONA su buffer al doblar,
     * y esa es justo la forma que una arena no puede servir bien.  Declararlo
     * es lo que permite contrastarlo despues con lo que se midio. */
    const util::AllocScope scope(
        util::AllocTag(util::AllocUse::Long, util::AllocShape::Growing));
    std::vector<std::string> names;
    for (std::size_t i = 0; i < n; ++i)
        names.push_back("simbolo_numero_" + std::to_string(i));
    g_sink = names.data();
}

void note_leftovers(std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {  // sin declarar nada, a proposito
        void *p = ::operator new(1024);
        g_sink = p;
        ::operator delete(p);
    }
}

} // namespace

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "informe_simbolos";

    parse_tokens(400);
    build_index(300);
    note_leftovers(200);

    /* LAS DOS LINEAS.  La primera dice quien sabe poner nombres; la segunda
     * pide el informe.  El resolutor lo trae la propia libreria. */
    util::alloc_set_symbol_resolver(vesta_self_resolver);
    if (!util::write_alloc_csv(dir)) {
        std::fprintf(stderr, "no se pudo escribir el informe en '%s'\n", dir);
        return 1;
    }

    /* Y se comprueba que de verdad salieron NOMBRES.  Un informe con
     * desplazamientos tambien se escribe sin error, asi que sin mirar esto el
     * ejemplo "funcionaria" igual sin demostrar nada. */
    util::SelfFrame frames[16];
    const unsigned n = util::self_inline_frames(
        reinterpret_cast<const void *>(&parse_tokens), frames, 16);
    std::printf("informe escrito en '%s'\n", dir);
    if (n == 0) {
        std::printf("PERO SIN NOMBRES: este binario no trae informacion de "
                    "depuracion.  Compila con `-g` y vuelve a probar.\n");
        return 0;
    }
    std::printf("y con nombres -- `parse_tokens` se resuelve en %u marco(s):\n",
                n);
    for (unsigned i = 0; i < n; ++i)
        std::printf("   %-9s %s%s%s\n", frames[i].inlined ? "[inline]" : "[real]",
                    frames[i].function != nullptr ? frames[i].function : "?",
                    frames[i].file != nullptr ? "   " : "",
                    frames[i].file != nullptr ? frames[i].file : "");
    std::printf("\nsi `sites.csv` esta vacio, faltó VESTA_HOST_ALLOC_SITES=1:\n"
                "apuntar de donde viene cada reserva cuesta, y no se paga sin "
                "pedirlo.\n");
    return 0;
}

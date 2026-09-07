/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file examples/c_symbol_report.c
 * @brief El MISMO informe con nombres, en C puro.
 *
 *     VESTA_HOST_ALLOC_SITES=1 vesta_alloc_example_c_symbol_report [carpeta]
 *
 * POR QUE EXISTE ESTE GEMELO.  Porque "la libreria sabe poner nombres a sus
 * propias direcciones" valdria la mitad si fuera una capacidad de C++.  Este
 * fichero hace lo mismo que `symbol_report.cpp` -- las mismas dos lineas de
 * enganche -- y se compila COMO C, que es lo unico que comprueba que las
 * cabeceras lo sean de verdad: el dia que a una se le cuele un `bool`, un
 * argumento por defecto o un namespace, esto deja de compilar en vez de
 * esperar a que lo descubra alguien de fuera.
 *
 * Y no es un caso rebuscado: las librerias en C que un proyecto lleva dentro
 * -- una de compresion, una de base de datos, una de criptografia -- reservan
 * por `malloc`, que en esta libreria YA es este asignador, y sus reservas
 * salen en el mismo informe que las demas.
 */

#include "util/report/alloc_csv_c.h"
#include "util/report/alloc_sites_c.h"
#include "util/alloc/host_allocator_c.h"
#include "util/symbols/self_inspect_c.h"
#include "util/symbols/self_resolver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Los punteros tienen que ESCAPAR o el compilador borra el par: `malloc` y
 * `free` no son funciones cualesquiera para el, conoce lo que hacen, y un par
 * cuyo resultado nadie lee lo elimina entero.  Sin esto el informe sale vacio
 * y parece que el asignador no ve nada. */
static void *volatile g_sink;

/* Tres sitios distintos, que es lo que el informe tiene que saber separar. */

static void parse_tokens(size_t n) {
    size_t i;
    for (i = 0; i < n; ++i) {
        void *p = malloc(96);
        g_sink = p;
        free(p);
    }
}

static void build_index(size_t n) {
    size_t i;
    for (i = 0; i < n; ++i) {
        void *p = calloc(n, sizeof(int));
        g_sink = p;
        free(p);
    }
}

static void note_leftovers(size_t n) {
    size_t i;
    for (i = 0; i < n; ++i) {
        void *p = malloc(1024);
        g_sink = p;
        free(p);
    }
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "informe_simbolos_c";
    VestaAllocFrame frames[16];
    unsigned n, i;

    parse_tokens(400);
    build_index(60);
    note_leftovers(200);

    /* LAS DOS LINEAS, iguales que en C++.  El resolutor lo trae la libreria y
     * su firma es la del gancho, asi que se enchufa tal cual. */
    vesta_alloc_set_symbol_resolver(vesta_self_resolver);
    if (!vesta_alloc_write_csv(dir)) {
        fprintf(stderr, "no se pudo escribir el informe en '%s'\n", dir);
        return 1;
    }

    /* Y se comprueba que salieron NOMBRES: un informe con desplazamientos se
     * escribe igual de bien, asi que sin esto el ejemplo pasaria sin demostrar
     * nada. */
    printf("informe escrito en '%s'\n", dir);
    n = vesta_self_resolver((const void *)(size_t)&parse_tokens, frames, 16);
    if (n == 0) {
        printf("PERO SIN NOMBRES: este binario no trae informacion de "
               "depuracion.  Compila con `-g` y vuelve a probar.\n");
        return 0;
    }
    printf("y con nombres -- `parse_tokens` se resuelve en %u marco(s):\n", n);
    for (i = 0; i < n; ++i) {
        printf("   %-9s %s%s%s\n", frames[i].inlined ? "[inline]" : "[real]",
               frames[i].function != NULL ? frames[i].function : "?",
               frames[i].file != NULL ? "   " : "",
               frames[i].file != NULL ? frames[i].file : "");
    }

    /* ---------------------------------------------------------------------
     *  Y EL INFORME ENTERO, sin escribir un solo fichero.
     *
     *  Esto es lo que demuestra la paridad: la foto de sitios, los contadores
     *  y los nombres son lo MISMO que ve un programa en C++, no un resumen.
     * ------------------------------------------------------------------- */
    {
        VestaHostAllocStats st;
        VestaAllocSite sites[16];
        unsigned kept, s, b;

        vesta_host_stats(&st);
        printf("\n%llu reservas pequenas, %llu grandes, %llu KiB comprometidos\n",
               (unsigned long long)st.small_allocs,
               (unsigned long long)st.large_allocs,
               (unsigned long long)(st.bytes_reserved / 1024));

        kept = vesta_alloc_sites_snapshot(sites, 16);
        if (kept == 0) {
            printf("\nsin sitios apuntados: falto VESTA_HOST_ALLOC_SITES=1.\n"
                   "Apuntar de donde viene cada reserva cuesta, y no se paga "
                   "sin pedirlo.\n");
            return 0;
        }
        printf("\nde donde vienen (%u sitios):\n", kept);
        for (s = 0; s < kept; ++s) {
            /* El nombre, aqui mismo: la libreria resuelve sus propias
             * direcciones y C puede pedirselo igual que C++. */
            const unsigned f = vesta_self_frames(sites[s].pc, frames, 16);
            const char *who = f > 0 && frames[f - 1].function != NULL
                                  ? frames[f - 1].function
                                  : "(sin nombre)";
            printf("  %6llu reservas  %8llu bytes  %-14s %s\n",
                   (unsigned long long)(sites[s].count - sites[s].over),
                   (unsigned long long)sites[s].bytes,
                   vesta_alloc_tag_name(sites[s].tag), who);
            /* Y de que TAMANO eran, que es lo que decide si a ese sitio le
             * conviene una arena.  La mascara dice que clases toca; esto,
             * cuantas de cada una. */
            for (b = 0; b < VESTA_ALLOC_SIZE_BUCKETS; ++b)
                if (sites[s].size_hist[b] != 0)
                    printf("            casilla %2u: %u\n", b,
                           sites[s].size_hist[b]);
        }
        if (vesta_alloc_sites_overflow() != 0)
            printf("\nOJO: hubo desalojos, asi que esas cuentas son cotas "
                   "inferiores.\n");
    }
    return 0;
}

/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file util/report/alloc_sites_c.h
 * @brief
 * \~english Where the allocations come from, with the signatures C
 *          understands.
 * \~spanish De donde vienen las reservas, con las firmas que entiende C.
 * \~
 *
 * \~english
 * PARITY, NOT A SUBSET.  The same thing a C++ program can see, a C one sees:
 * the whole picture of sites, with their counts, their purpose, their size
 * split and their upper bound.  In this project the C++ CALLS C code and
 * several dependencies are pure C; a capability that only exists in C++ is a
 * capability half the program does not have.
 *
 * ONE SINGLE DEFINITION OF THE STRUCTURE.  It is declared HERE and the C++ side
 * names it again with `using`.  Declaring it twice -- once per language -- is
 * the kind of duplication nobody sees break: the day a field is added to one,
 * the other still compiles and starts reading a different structure.
 *
 * \~spanish
 * PARIDAD, NO UN SUBCONJUNTO.  Lo mismo que puede ver un programa en C++ lo ve
 * uno en C: la foto de sitios entera, con sus cuentas, su proposito, su
 * reparto de tamanos y su cota superior.  En este proyecto el C++ LLAMA a
 * codigo C y varias dependencias son C puro; una capacidad que solo existe en
 * C++ es una capacidad que la mitad del programa no tiene.
 *
 * UNA SOLA DEFINICION DE LA ESTRUCTURA.  Se declara AQUI y el lado de C++ la
 * nombra otra vez con `using`.  Declararla dos veces -- una por lenguaje -- es
 * la duplicacion que nadie ve romperse: el dia que se le anade un campo a una,
 * la otra sigue compilando y empieza a leer una estructura distinta.
 *
 * \~
 */
#ifndef VESTA_UTIL_ALLOC_SITES_C_H
#define VESTA_UTIL_ALLOC_SITES_C_H

/* Las etiquetas -- sus valores y cuantas ranuras ocupan -- viven en su propia
 * cabecera, que comparten C y C++.  Aqui se usan; no se redefinen. */
#include "util/alloc/alloc_tag_c.h"

#include <stddef.h>
#include <stdint.h>

/**
 * @brief
 * \~english How many cells the size split has.
 * \~spanish Cuantas casillas tiene el reparto de tamanos.
 * \~
 *
 * \~english
 * The number lives here because the structure below needs it and C cannot read
 * the `constexpr` in `util/alloc/size_buckets.h`.  It is not a second source:
 * the C++ side CHECKS that it matches `kSizeBuckets` and does not compile if it
 * does not -- see the `static_assert` in `util/report/alloc_sites.h`.
 *
 * \~spanish
 * El numero vive aqui porque la estructura de abajo lo necesita y C no puede
 * leer el `constexpr` de `util/alloc/size_buckets.h`.  No es una segunda fuente: el
 * lado de C++ COMPRUEBA que coincide con `kSizeBuckets` y no compila si no --
 * ver el `static_assert` de `util/report/alloc_sites.h`.
 *
 * \~
 */
#define VESTA_ALLOC_SIZE_BUCKETS 12

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief
 * \~english A site that allocated, and what it did.
 * \~spanish Un sitio que reservo, y que hizo.
 * \~
 *
 * \~english
 * `count` is an UPPER BOUND and `over` is what it inherited when it evicted a
 * weaker entry: the truth is in `[count - over, count]`.  They travel apart on
 * purpose -- putting them together would give a number that looks exact and is
 * not -- and the floor is what that site really earned.
 *
 * \~spanish
 * `count` es una COTA SUPERIOR y `over` lo que heredo al desalojar a una
 * entrada mas debil: lo cierto esta en `[count - over, count]`.  Viajan
 * separados a proposito -- juntarlos daria un numero que parece exacto y no lo
 * es --, y el suelo es lo que ese sitio se gano de verdad.
 *
 * \~
 */
typedef struct VestaAllocSite {
    /**
     * \~english the address as it is LOADED
     * \~spanish la direccion tal como esta CARGADA
     * \~
     */
    const void *pc;
    /**
     * \~english allocations: an upper bound.  See `over`
     * \~spanish reservas: cota superior.  Ver `over`
     * \~
     */
    uint64_t count;
    /**
     * \~english bytes asked for: an upper bound
     * \~spanish bytes pedidos: cota superior
     * \~
     */
    uint64_t bytes;
    /**
     * \~english one bit per size class touched
     * \~spanish un bit por clase de tamano tocada
     * \~
     */
    uint64_t class_mask;
    /**
     * \~english how many went past the class cap
     * \~spanish cuantas pasaban del tope de clases
     * \~
     */
    uint64_t large;
    /**
     * \~english what was inherited on eviction; zero = an exact count
     * \~spanish lo heredado al desalojar; cero = cuenta exacta
     * \~
     */
    uint64_t over;
    /**
     * \~english the same for `bytes`
     * \~spanish lo mismo para `bytes`
     * \~
     */
    uint64_t over_bytes;
    /**
     * \~english the purpose, packed.  It is part of the key
     * \~spanish el proposito, empaquetado.  Forma parte de la clave
     * \~
     */
    uint8_t tag;
    /**
     * @brief
     * \~english How many allocations of each size THIS site made.
     * \~spanish Cuantas reservas de cada tamano hizo ESTE sitio.
     * \~
     *
     * \~english
     * The mask above says WHICH sizes it touches; this one, HOW MANY of each.
     * They are different questions: a site with a million 32-byte allocations
     * and one of 16 MiB has the same mask as one that does exactly the
     * opposite, and the average does not tell them apart either.
     *
     * \~spanish
     * La mascara de arriba dice QUE tamanos toca; esto, CUANTAS de cada uno.
     * Son preguntas distintas: un sitio con un millon de reservas de 32 bytes
     * y una de 16 MiB tiene la misma mascara que uno que hace justo al reves,
     * y con la media tampoco se distinguen.
     *
     * \~
     */
    uint32_t size_hist[VESTA_ALLOC_SIZE_BUCKETS];
    /**
     * @brief
     * \~english How many BYTES of each size THIS site asked for.
     * \~spanish Cuantos BYTES de cada tamano pidio ESTE sitio.
     * \~
     *
     * \~english
     * The one above counts allocations, and counting is what hides the thing
     * this is for: a bucket holds a RANGE of sizes, so the count of the last
     * one -- everything above 16 MiB -- is a handful next to tens of millions
     * of tiny ones and comes out as a bar of zero width, while in bytes it can
     * be the one that decides the peak.  Measured on a compile of 441.000
     * lines: 32 allocations over 16 MiB out of 89,9 million, 0,00004% of the
     * count.
     *
     * It cannot be worked out from the count.  A bucket spans a factor of four,
     * so counts give a range and never a figure, and the last bucket has no
     * ceiling at all -- from there only a floor comes out.
     *
     * \~spanish
     * El de arriba cuenta reservas, y contar es justo lo que esconde aquello
     * para lo que esta esto: una casilla abarca un RANGO de tamanos, asi que la
     * cuenta de la ultima -- todo lo que pasa de 16 MiB -- es un punado al lado
     * de decenas de millones de diminutas y sale como una barra de ancho cero,
     * cuando en bytes puede ser la que decide el pico.  Medido sobre una
     * compilacion de 441.000 lineas: 32 reservas de mas de 16 MiB entre 89,9
     * millones, el 0,00004% de la cuenta.
     *
     * No se puede deducir de la cuenta.  Una casilla abarca un factor de
     * cuatro, asi que de un recuento sale un intervalo y nunca una cifra, y la
     * ultima casilla ni siquiera tiene techo -- de ahi solo sale un suelo.
     *
     * \~
     */
    uint64_t bytes_hist[VESTA_ALLOC_SIZE_BUCKETS];
} VestaAllocSite;

/**
 * @brief
 * \~english Copies out the sites that allocate most, ordered from most to
 *          least.
 * \~spanish Copia los sitios que mas reservan, ordenados de mayor a menor.
 * \~
 *
 * \~english
 * The threads are added up before sorting: the same site seen from eight
 * threads is ONE site, not eight.
 *
 * \~spanish
 * Se suman los hilos antes de ordenar: un mismo sitio visto desde ocho hilos
 * es UN sitio, no ocho.
 *
 * \~
 * @param out
 * \~english the caller's array.  This library does NOT allocate here: it is
 *           called from the allocation path and from the end of the process,
 *           and in both places asking for memory is how a hang is reached.
 * \~spanish array del que llama.  Esta libreria NO reserva aqui: se la llama
 *           desde el camino de reservar y desde el final del proceso, y en los
 *           dos sitios pedir memoria es como se llega a un cuelgue.
 * \~
 * @param max
 * \~english how many fit in @p out.
 * \~spanish cuantos caben en @p out.
 * \~
 * @return
 * \~english how many were written.
 * \~spanish cuantos se escribieron.
 * \~
 *
 * \~english
 * @code
 *   VestaAllocSite top[16];
 *   const unsigned n = vesta_alloc_sites_snapshot(top, 16);
 *   for (unsigned i = 0; i < n; ++i)
 *       printf("%p %llu\n", top[i].pc, (unsigned long long)top[i].count);
 * @endcode
 *
 * \~spanish
 * @code
 *   VestaAllocSite top[16];
 *   const unsigned n = vesta_alloc_sites_snapshot(top, 16);
 *   for (unsigned i = 0; i < n; ++i)
 *       printf("%p %llu\n", top[i].pc, (unsigned long long)top[i].count);
 * @endcode
 *
 * \~
 */
unsigned vesta_alloc_sites_snapshot(VestaAllocSite *out, unsigned max);

/**
 * @brief
 * \~english Prints the @p top sites that allocate most on `stderr`.
 * \~spanish Imprime en `stderr` los @p top sitios que mas reservan.
 * \~
 *
 * \~english
 * It is the one-line report: without writing files and reading them back
 * afterwards.  They come out as OFFSETS from the module base, which is the only
 * thing that can be read with the binary in another run -- the address as it
 * stands changes with the random layout.
 *
 * Whoever wants names installs the resolver
 * (`util/symbols/self_resolver.h`) and exports with `vesta_alloc_write_csv`.
 *
 * \~spanish
 * Es el informe de una linea: sin escribir ficheros ni leerlos despues.  Salen
 * como DESPLAZAMIENTOS desde la base del modulo, que es lo unico que se puede
 * leer con el binario en otra corrida -- la direccion tal cual cambia con la
 * disposicion aleatoria.
 *
 * Quien quiera nombres instala el resolutor (`util/symbols/self_resolver.h`) y exporta
 * con `vesta_alloc_write_csv`.
 *
 * \~
 * @param top
 * \~english how many to show, from the one that allocates most downwards.
 * \~spanish cuantos ensenar, del que mas reserva hacia abajo.
 * \~
 */
void vesta_alloc_dump_sites(unsigned top);

/**
 * @brief
 * \~english Notes down by hand that something was allocated at @p pc.
 * \~spanish Apunta a mano que se reservo en @p pc.
 * \~
 *
 * \~english
 * FOR WHOM.  For a library that serves its own memory through another door --
 * an arena of its own, a block handed out by hand -- and wants to show up in
 * the same report as everything else.  What goes through `malloc` or through
 * `operator new` is already noted down on its own.
 *
 * \~spanish
 * PARA QUIEN.  Para una libreria que sirva su propia memoria por otra puerta
 * -- una arena suya, un bloque que se reparte a mano -- y quiera aparecer en
 * el mismo informe que todo lo demas.  Lo que pasa por `malloc` o por
 * `operator new` ya se apunta solo.
 *
 * \~
 * @param pc
 * \~english the address that identifies the site.
 * \~spanish la direccion que identifica el sitio.
 * \~
 * @param n
 * \~english the bytes that were handed out.
 * \~spanish los bytes que se entregaron.
 * \~
 * @param tag
 * \~english the packed purpose; zero when nothing is declared.
 * \~spanish el proposito empaquetado; cero si no se declara.
 * \~
 */
void vesta_alloc_record_site(const void *pc, size_t n, unsigned char tag);

/**
 * @brief
 * \~english How many times an entry was evicted to make room for another.
 * \~spanish Cuantas veces se desalojo una entrada para meter otra.
 * \~
 * @return
 * \~english the count; if it is not zero, the sites' figures are upper bounds
 *           and have to be read as such.
 * \~spanish la cuenta; si no es cero, las cuentas de los sitios son cotas
 *           superiores y hay que leerlas como tales.
 * \~
 */
uint64_t vesta_alloc_sites_overflow(void);

/**
 * @brief
 * \~english How many allocations could not be noted down for want of anywhere
 *          to do it.
 * \~spanish Cuantas reservas no se pudieron apuntar por no haber sitio donde
 *          hacerlo.
 * \~
 * @return
 * \~english the count, normally zero.
 * \~spanish la cuenta, normalmente cero.
 * \~
 */
uint64_t vesta_alloc_sites_skipped(void);

/**
 * @brief
 * \~english The name of a packed purpose, for printing it.
 * \~spanish El nombre de un proposito empaquetado, para imprimirlo.
 * \~
 * @param tag
 * \~english the packed purpose.
 * \~spanish el proposito empaquetado.
 * \~
 * @return
 * \~english constant text, never NULL.  The combinations that are not used come
 *           out as "-", which is distinct from "unknown".
 * \~spanish texto constante, nunca NULL.  Las combinaciones que no se usan
 *           salen como "-", que se distingue de "unknown".
 * \~
 */
const char *vesta_alloc_tag_name(unsigned char tag);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VESTA_UTIL_ALLOC_SITES_C_H */

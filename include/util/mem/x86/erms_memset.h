/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/mem/x86/erms_memset.h
 * @brief Rellenar con @c rep @c stosb, dejando que lo haga el microcodigo.
 *
 * El companero de @c erms_memcpy.h, y todo lo que se cuenta alli vale aqui: es
 * UNA instruccion que le dice al procesador "escribe n bytes" y le deja elegir
 * el ancho, con arranque caro y por eso con umbral.
 *
 * El umbral es OTRO, y no por gusto: rellenar solo escribe, asi que el bucle
 * vectorial propio le saca mas partido a la cache que en una copia y
 * @c rep @c stosb tarda mas en compensar.  Sale de medir, en
 * @c bench_memset.
 *
 * @warning Solo se puede llamar si @c vesta_mem_x86_has_erms().
 */
#ifndef VESTA_UTIL_MEM_X86_ERMS_MEMSET_H
#define VESTA_UTIL_MEM_X86_ERMS_MEMSET_H

#include "util/mem/x86/x86_cpu.h"

#if defined(VESTA_MEM_ARCH_X86)

/**
 * @brief A partir de cuantos bytes compensa @c rep @c stosb.
 *
 * MUY POR ENCIMA del umbral de la copia, y no es una errata.  MEDIDO en
 * `bench_memset` (ns por relleno, GCC 15 / Linux):
 *
 *          1 KiB    4 KiB    64 KiB     1 MiB
 *   con      8,72    13,63     811      12.709
 *   sin      3,43    12,60     800      15.257
 *   glibc    3,25    13,76     802      12.970
 *
 * Hasta 64 KiB no gana NADA -- a 1 KiB pierde por 2,5x y a 4 KiB y 64 KiB
 * empata --, y solo despega en 1 MiB, donde gana un 20% y pasa a glibc.  La
 * razon es que rellenar SOLO ESCRIBE: el bucle vectorial propio ya satura el
 * ancho de banda de escritura mientras el destino quepa en cache, y lo que
 * aporta la instruccion -- que el microcodigo se salte la cache -- solo importa
 * cuando ya no cabe.  Copiar ademas lee, y ahi cruza mucho antes.
 *
 * PERO SE PONE EN 4 KiB, no en 128, y la razon es la mejor leccion de todo
 * esto: el bucle propio rinde SEGUN EL COMPILADOR y esta instruccion no.  El
 * mismo fuente, mismo procesador, relleno de 4 KiB:
 *
 *              bucle propio    rep stosb
 *   GCC 15        12,60 ns       13,63 ns     <- el bucle gana por un 8%
 *   GCC 10        34,45 ns       15,40 ns     <- el bucle pierde por 2,2x
 *
 * Con el umbral alto se elige el bucle, y eso deja al toolchain viejo -- que es
 * el que usa medio mundo en Windows -- perdiendo 2,2x a 4 KiB y 1,8x a 64 KiB.
 * Bajarlo cuesta un 8% donde el compilador es bueno y evita un 120% donde no lo
 * es.  Un umbral se elige para el peor caso razonable, no para el mejor.
 */
#if !defined(VESTA_MEM_ERMS_MIN_FILL)
#define VESTA_MEM_ERMS_MIN_FILL 4096
#endif

/**
 * @brief Pone @p n bytes al valor @p v con @c rep @c stosb.
 *
 * @param d Destino.
 * @param v Byte a repetir.  Va en @c al.
 * @param n Cuantos bytes.
 *
 * @par Hilos
 * Segura, mientras el bufer sea de quien llama.
 *
 * @code
 *   if (vesta_mem_x86_has_erms()) vesta_mem_erms_fill(dst, 0, n);
 * @endcode
 */
VESTA_MEM_ALWAYS_INLINE void vesta_mem_erms_fill(uint8_t *d, uint8_t v,
                                                 size_t n) VESTA_MEM_NOEXCEPT {
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(v) : "memory");
}

#endif // VESTA_MEM_ARCH_X86

#endif // VESTA_UTIL_MEM_X86_ERMS_MEMSET_H

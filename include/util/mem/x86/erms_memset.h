/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/mem/x86/erms_memset.h
 * @brief
 * \~english Filling with @c rep @c stosb, letting the microcode do it.
 * \~spanish Rellenar con @c rep @c stosb, dejando que lo haga el microcodigo.
 * \~
 *
 * \~english
 * The companion of @c erms_memcpy.h, and everything told there applies here: it
 * is ONE instruction that tells the processor "write n bytes" and lets it
 * choose the width, with an expensive start-up and therefore with a threshold.
 *
 * The threshold is a DIFFERENT one, and not out of preference: filling only
 * writes, so our own vector loop gets more out of the cache than in a copy and
 * @c rep @c stosb takes longer to pay.  It comes out of measuring, in
 * @c bench_memset.
 *
 * @warning It can only be called if @c vesta_mem_x86_has_erms().
 *
 * \~spanish
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
 *
 * \~
 */
#ifndef VESTA_UTIL_MEM_X86_ERMS_MEMSET_H
#define VESTA_UTIL_MEM_X86_ERMS_MEMSET_H

#include "util/mem/x86/x86_cpu.h"

#if defined(VESTA_MEM_ARCH_X86)

/**
 * @brief
 * \~english From how many bytes on @c rep @c stosb pays.
 * \~spanish A partir de cuantos bytes compensa @c rep @c stosb.
 * \~
 *
 * \~english
 * FAR ABOVE the copy's threshold, and it is not a typo.  MEASURED in
 * `bench_memset` (ns per fill, GCC 15 / Linux):
 *
 *            1 KiB    4 KiB    64 KiB     1 MiB
 *   with      8.72    13.63       811     12,709
 *   without   3.43    12.60       800     15,257
 *   glibc     3.25    13.76       802     12,970
 *
 * Up to 64 KiB it gains NOTHING -- at 1 KiB it loses by 2.5x and at 4 KiB and
 * 64 KiB it draws -- and it only takes off at 1 MiB, where it wins 20% and
 * passes glibc.  The reason is that filling ONLY WRITES: our own vector loop
 * already saturates the write bandwidth while the destination fits in cache,
 * and what the instruction contributes -- the microcode skipping the cache --
 * only matters once it no longer fits.  Copying also reads, and there it
 * crosses much earlier.
 *
 * BUT IT IS SET AT 4 KiB, not at 128, and the reason is the best lesson in all
 * of this: our own loop performs ACCORDING TO THE COMPILER and this instruction
 * does not.  The same source, the same processor, a 4 KiB fill:
 *
 *                our own loop    rep stosb
 *   GCC 15          12.60 ns      13.63 ns    <- the loop wins by 8%
 *   GCC 10          34.45 ns      15.40 ns    <- the loop loses by 2.2x
 *
 * With a high threshold the loop gets picked, and that leaves the old toolchain
 * -- which is what half the world uses on Windows -- losing 2.2x at 4 KiB and
 * 1.8x at 64 KiB.  Lowering it costs 8% where the compiler is good and avoids
 * 120% where it is not.  A threshold is chosen for the worst reasonable case,
 * not for the best.
 *
 * \~spanish
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
 *
 * \~
 */
#if !defined(VESTA_MEM_ERMS_MIN_FILL)
#define VESTA_MEM_ERMS_MIN_FILL 4096
#endif

/**
 * @brief
 * \~english Sets @p n bytes to the value @p v with @c rep @c stosb.
 * \~spanish Pone @p n bytes al valor @p v con @c rep @c stosb.
 * \~
 *
 * \~english
 * @par Threads
 * Safe, as long as the buffer belongs to the caller.
 *
 * \~spanish
 * @par Hilos
 * Segura, mientras el bufer sea de quien llama.
 *
 * \~
 * @param d
 * \~english the destination.
 * \~spanish destino.
 * \~
 * @param v
 * \~english the byte to repeat.  It goes in @c al.
 * \~spanish byte a repetir.  Va en @c al.
 * \~
 * @param n
 * \~english how many bytes.
 * \~spanish cuantos bytes.
 * \~
 *
 * \~english
 * @code
 *   if (vesta_mem_x86_has_erms()) vesta_mem_erms_fill(dst, 0, n);
 * @endcode
 *
 * \~spanish
 * @code
 *   if (vesta_mem_x86_has_erms()) vesta_mem_erms_fill(dst, 0, n);
 * @endcode
 *
 * \~
 */
VESTA_MEM_ALWAYS_INLINE void vesta_mem_erms_fill(uint8_t *d, uint8_t v,
                                                 size_t n) VESTA_MEM_NOEXCEPT {
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(v) : "memory");
}

#endif // VESTA_MEM_ARCH_X86

#endif // VESTA_UTIL_MEM_X86_ERMS_MEMSET_H

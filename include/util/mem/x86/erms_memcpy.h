/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/mem/x86/erms_memcpy.h
 * @brief
 * \~english Copying with @c rep @c movsb, letting the microcode do it.
 * \~spanish Copiar con @c rep @c movsb, dejando que lo haga el microcodigo.
 * \~
 *
 * \~english
 * WHAT THIS IS AND WHY IT IS DIFFERENT FROM THE REST.  The other micro-ISAs are
 * loops we write; this one is ONE instruction that tells the processor "copy n
 * bytes" and lets it choose how.  From ERMS on (Enhanced REP MOVSB, Ivy Bridge
 * onwards) the microcode resolves it with the widest size the machine has,
 * skipping the cache hierarchy when it suits and without dirtying it -- things
 * that cannot be done from here, because they depend on the particular model.
 *
 * WHY IT WAS NEEDED, measured: in `bench_memcpy`, a 4 KiB copy cost us 43 ns
 * against glibc's 22, on BOTH platforms and with BOTH compilers.  The result
 * not changing with the compiler is what identifies the gap as one of
 * ALGORITHM and not of code generation: glibc has this path and we did not.
 *
 * AND WHY IT IS NOT ALWAYS USED.  @c rep @c movsb has an expensive start-up --
 * on the order of tens of cycles -- so below the threshold it loses to a couple
 * of vector moves.  The threshold is in @c VESTA_MEM_ERMS_MIN_COPY, and it
 * comes out of measuring, not out of the documentation.
 *
 * @warning It can only be called if @c vesta_mem_x86_has_erms().  Without ERMS
 *          the instruction exists all the same -- it comes from the 8086 -- but
 *          it goes byte by byte and is MUCH slower than any other path here.
 *
 * \~spanish
 * QUE ES ESTO Y POR QUE ES DISTINTO DE LO DEMAS.  Las otras micro-ISA son
 * bucles que escribimos nosotros; esta es UNA instruccion que le dice al
 * procesador "copia n bytes" y le deja elegir como.  A partir de ERMS
 * (Enhanced REP MOVSB, de Ivy Bridge en adelante) el microcodigo la resuelve
 * con el ancho mayor que la maquina tenga, saltandose la jerarquia de cache
 * cuando le conviene y sin ensuciarla -- cosas que desde aqui no se pueden
 * hacer, porque dependen del modelo concreto.
 *
 * POR QUE HACIA FALTA, medido: en `bench_memcpy`, una copia de 4 KiB nos
 * costaba 43 ns contra los 22 de glibc, en las DOS plataformas y con los DOS
 * compiladores.  Que el resultado no cambie con el compilador es lo que
 * identifica el hueco como de ALGORITMO y no de generacion de codigo: glibc
 * tiene este camino y nosotros no.
 *
 * Y POR QUE NO SE USA SIEMPRE.  @c rep @c movsb tiene un arranque caro -- del
 * orden de decenas de ciclos --, asi que por debajo del umbral pierde contra un
 * par de movimientos vectoriales.  El umbral esta en
 * @c VESTA_MEM_ERMS_MIN_COPY, y sale de medir, no de la documentacion.
 *
 * @warning Solo se puede llamar si @c vesta_mem_x86_has_erms().  Sin ERMS la
 *          instruccion existe igual -- viene del 8086 -- pero va byte a byte y
 *          es MUCHO mas lenta que cualquier otro camino de aqui.
 *
 * \~
 */
#ifndef VESTA_UTIL_MEM_X86_ERMS_MEMCPY_H
#define VESTA_UTIL_MEM_X86_ERMS_MEMCPY_H

#include "util/mem/x86/x86_cpu.h"

#if defined(VESTA_MEM_ARCH_X86)

/**
 * @brief
 * \~english From how many bytes on @c rep @c movsb pays.
 * \~spanish A partir de cuantos bytes compensa @c rep @c movsb.
 * \~
 *
 * \~english
 * MEASURED, sweeping the threshold in `bench_memcpy` (ns per copy, GCC 15 /
 * Linux):
 *
 *            1 KiB    4 KiB    64 KiB     1 MiB
 *   with     11.15    22.15       965     21,879
 *   without   9.85    42.30     1,094     26,733
 *   glibc     5.59    21.57       946     22,847
 *
 * At 1 KiB it still LOSES -- the instruction's start-up costs more than the
 * whole copy -- and from 4 KiB on it wins by almost double.  Hence the cut at
 * 2 KiB, which is where it crosses.  Above that we follow glibc closely and at
 * 1 MiB we pass it.
 *
 * It can be set from outside to re-tune it on another machine without touching
 * the code.
 *
 * \~spanish
 * MEDIDO, barriendo el umbral en `bench_memcpy` (ns por copia, GCC 15 / Linux):
 *
 *          1 KiB    4 KiB    64 KiB     1 MiB
 *   con     11,15    22,15     965      21.879
 *   sin      9,85    42,30    1.094     26.733
 *   glibc    5,59    21,57     946      22.847
 *
 * A 1 KiB todavia PIERDE -- el arranque de la instruccion cuesta mas que la
 * copia entera -- y a partir de 4 KiB gana casi el doble.  De ahi el corte en
 * 2 KiB, que es donde cruza.  Por encima seguimos a glibc de cerca y en 1 MiB
 * la pasamos.
 *
 * Se puede fijar desde fuera para reafinarlo en otra maquina sin tocar el
 * codigo.
 *
 * \~
 */
#if !defined(VESTA_MEM_ERMS_MIN_COPY)
#define VESTA_MEM_ERMS_MIN_COPY 2048
#endif

/**
 * @brief
 * \~english Copies @p n bytes with @c rep @c movsb.
 * \~spanish Copia @p n bytes con @c rep @c movsb.
 * \~
 *
 * \~english
 * The three operands go in fixed registers and the instruction LEAVES them
 * advanced, which is why they go in and out (@c "+D", @c "+S", @c "+c").  The
 * memory clobber is mandatory: without it, the compiler does not know this
 * writes into @p d and can reorder or delete accesses around it.
 *
 * The direction is fixed by the ABI: the direction flag is zero on entry to and
 * exit from any function, so the copy goes forwards and it does not have to be
 * set or restored.
 *
 * @par Threads
 * Safe, as long as the buffers belong to the caller.
 *
 * \~spanish
 * Los tres operandos van en registros fijos y la instruccion los DEJA
 * avanzados, por eso entran y salen (@c "+D", @c "+S", @c "+c").  El clobber de
 * memoria es obligatorio: sin el, el compilador no sabe que esto escribe en
 * @p d y puede reordenar o borrar accesos alrededor.
 *
 * La direccion la fija el ABI: la bandera de direccion esta a cero al entrar y
 * al salir de cualquier funcion, asi que la copia va hacia adelante y no hace
 * falta ni ponerla ni restaurarla.
 *
 * @par Hilos
 * Segura, mientras los bufers sean de quien llama.
 *
 * \~
 * @param d
 * \~english the destination.
 * \~spanish destino.
 * \~
 * @param s
 * \~english the source.  It must not overlap @p d.
 * \~spanish origen.  No puede solapar con @p d.
 * \~
 * @param n
 * \~english how many bytes.
 * \~spanish cuantos bytes.
 * \~
 *
 * \~english
 * @code
 *   if (vesta_mem_x86_has_erms()) vesta_mem_erms_copy(dst, src, n);
 * @endcode
 *
 * \~spanish
 * @code
 *   if (vesta_mem_x86_has_erms()) vesta_mem_erms_copy(dst, src, n);
 * @endcode
 *
 * \~
 */
VESTA_MEM_ALWAYS_INLINE void vesta_mem_erms_copy(uint8_t *d, const uint8_t *s,
                                                 size_t n) VESTA_MEM_NOEXCEPT {
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
}

#endif // VESTA_MEM_ARCH_X86

#endif // VESTA_UTIL_MEM_X86_ERMS_MEMCPY_H

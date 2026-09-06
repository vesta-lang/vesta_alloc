/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/mem/x86/erms_memcpy.h
 * @brief Copiar con @c rep @c movsb, dejando que lo haga el microcodigo.
 *
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
 */
#ifndef VESTA_UTIL_MEM_X86_ERMS_MEMCPY_H
#define VESTA_UTIL_MEM_X86_ERMS_MEMCPY_H

#include "util/mem/x86/x86_cpu.h"

#if defined(VESTA_MEM_ARCH_X86)

/**
 * @brief A partir de cuantos bytes compensa @c rep @c movsb.
 *
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
 */
#if !defined(VESTA_MEM_ERMS_MIN_COPY)
#define VESTA_MEM_ERMS_MIN_COPY 2048
#endif

/**
 * @brief Copia @p n bytes con @c rep @c movsb.
 *
 * Los tres operandos van en registros fijos y la instruccion los DEJA
 * avanzados, por eso entran y salen (@c "+D", @c "+S", @c "+c").  El clobber de
 * memoria es obligatorio: sin el, el compilador no sabe que esto escribe en
 * @p d y puede reordenar o borrar accesos alrededor.
 *
 * La direccion la fija el ABI: la bandera de direccion esta a cero al entrar y
 * al salir de cualquier funcion, asi que la copia va hacia adelante y no hace
 * falta ni ponerla ni restaurarla.
 *
 * @param d Destino.
 * @param s Origen.  No puede solapar con @p d.
 * @param n Cuantos bytes.
 *
 * @par Hilos
 * Segura, mientras los bufers sean de quien llama.
 *
 * @code
 *   if (vesta_mem_x86_has_erms()) vesta_mem_erms_copy(dst, src, n);
 * @endcode
 */
VESTA_MEM_ALWAYS_INLINE void vesta_mem_erms_copy(uint8_t *d, const uint8_t *s,
                                                 size_t n) VESTA_MEM_NOEXCEPT {
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
}

#endif // VESTA_MEM_ARCH_X86

#endif // VESTA_UTIL_MEM_X86_ERMS_MEMCPY_H

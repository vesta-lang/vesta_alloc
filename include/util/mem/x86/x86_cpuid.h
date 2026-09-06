/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/mem/x86/x86_cpuid.h
 * @brief Las dos instrucciones con las que x86 se deja preguntar: @c CPUID y
 *        @c XGETBV.  Envueltas y nada mas.
 *
 * Aqui NO hay ninguna decision sobre capacidades: esto contesta lo que dice el
 * procesador, y quien interpreta esas banderas es @c util/mem/x86/x86_cpu.h.
 * Estan separadas porque son dos cosas distintas -- una es una instruccion, la
 * otra es una politica -- y porque asi las preguntas de capacidad se leen como
 * preguntas en vez de como cuatro registros y un ensamblador en linea.
 *
 * POR QUE NO SE USA @c <cpuid.h>.  La trae el compilador y hace justo esto,
 * pero define @c __cpuid como una MACRO de cinco parametros, y en Windows eso
 * choca con la declaracion @c __cpuid del @c <intrin.h> de MSVC, que entra sola
 * por cualquier cabecera de la biblioteca estandar.  Comprobado: con Clang
 * apuntando a MSVC, incluir las dos rompe la compilacion con
 * "too few arguments provided to function-like macro".  Una libreria que se
 * quiere poder incluir en cualquier sitio no puede traerse esa mina.
 *
 * @par Hilos
 * Las dos son seguras desde cualquier hilo: leen estado del procesador y no
 * tocan memoria.
 */
#ifndef VESTA_UTIL_MEM_X86_CPUID_H
#define VESTA_UTIL_MEM_X86_CPUID_H

#include "util/mem/mem_config.h"

#if defined(VESTA_MEM_ARCH_X86)

/// Los cuatro registros que devuelve @c CPUID, en su orden de siempre.
typedef struct {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
} vesta_cpuid_regs;

/**
 * @brief Ejecuta @c CPUID sobre la hoja @p leaf y la subhoja @p subleaf.
 *
 * El baile de @c ebx no es adorno: al compilar codigo independiente de posicion
 * en 32 bits, @c ebx es el registro que guarda la tabla de desplazamientos
 * globales y el compilador NO deja usarlo como salida.  La forma con @c xchg es
 * la de siempre para ese caso.  En 64 bits no hay tal reserva y se puede pedir
 * directo.
 *
 * @param leaf    Hoja.  La 0 devuelve en @c eax la mayor disponible.
 * @param subleaf Subhoja, en @c ecx.  Cero para las hojas que no la usan.
 * @return Los cuatro registros tal cual los deja el procesador.
 *
 * @par Hilos
 * Segura.  No toca memoria compartida.
 *
 * @code
 *   const vesta_cpuid_regs r = vesta_mem_x86_cpuid(1, 0);
 *   const int tiene_sse42 = (r.ecx & (1u << 20)) != 0;
 * @endcode
 */
VESTA_MEM_ALWAYS_INLINE vesta_cpuid_regs
vesta_mem_x86_cpuid(uint32_t leaf, uint32_t subleaf) VESTA_MEM_NOEXCEPT {
    vesta_cpuid_regs r;
#if defined(__i386__) && defined(__PIC__)
    __asm__ volatile("xchgl %%ebx, %1\n\t"
                     "cpuid\n\t"
                     "xchgl %%ebx, %1"
                     : "=a"(r.eax), "=&r"(r.ebx), "=c"(r.ecx), "=d"(r.edx)
                     : "a"(leaf), "c"(subleaf));
#else
    __asm__ volatile("cpuid"
                     : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx)
                     : "a"(leaf), "c"(subleaf));
#endif
    return r;
}

/**
 * @brief La mayor hoja de @c CPUID que entiende este procesador.
 *
 * Hay que preguntarlo ANTES de usar una hoja alta: pedir una que no existe no
 * da error, devuelve la ultima que si existe, y sus bits se leerian como si
 * fueran los de la que se pedia.
 *
 * @return El numero de la ultima hoja basica disponible.
 *
 * @par Hilos
 * Segura.
 *
 * @code
 *   if (vesta_mem_x86_cpuid_max() >= 7) { ... }
 * @endcode
 */
VESTA_MEM_ALWAYS_INLINE uint32_t
vesta_mem_x86_cpuid_max(void) VESTA_MEM_NOEXCEPT {
    return vesta_mem_x86_cpuid(0, 0).eax;
}

/**
 * @brief Lee un registro de control extendido con @c XGETBV.
 *
 * Es lo que contesta que estado GUARDA el sistema operativo al cambiar de
 * tarea, que es una pregunta distinta de que sabe hacer la CPU.
 *
 * @warning Solo se puede ejecutar si la hoja 1 trae OSXSAVE (bit 27 de @c ecx).
 *          Sin eso, la instruccion provoca una excepcion de instruccion no
 *          valida.  Quien llama comprueba primero.
 *
 * @param index Que registro.  El 0 es XCR0, el del estado extendido.
 * @return Los 64 bits del registro (@c edx:eax).
 *
 * @par Hilos
 * Segura.
 *
 * @code
 *   // bits 1 y 2: el sistema guarda XMM e YMM
 *   const int ymm_ok = (vesta_mem_x86_xgetbv(0) & 0x6u) == 0x6u;
 * @endcode
 */
VESTA_MEM_ALWAYS_INLINE uint64_t
vesta_mem_x86_xgetbv(uint32_t index) VESTA_MEM_NOEXCEPT {
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(index));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

#endif // VESTA_MEM_ARCH_X86

#endif // VESTA_UTIL_MEM_X86_CPUID_H

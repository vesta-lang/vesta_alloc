/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/mem/x86/x86_cpuid.h
 * @brief
 * \~english The two instructions with which x86 lets itself be asked: @c CPUID
 *          and @c XGETBV.  Wrapped and nothing more.
 * \~spanish Las dos instrucciones con las que x86 se deja preguntar: @c CPUID y
 *          @c XGETBV.  Envueltas y nada mas.
 * \~
 *
 * \~english
 * There is NO decision about capabilities here: this reports what the processor
 * says, and who interprets those flags is @c util/mem/x86/x86_cpu.h.  They are
 * kept apart because they are two different things -- one is an instruction,
 * the other is a policy -- and because that way the capability questions read
 * like questions instead of like four registers and some inline assembly.
 *
 * WHY `<cpuid.h>` IS NOT USED.  The compiler ships it and it does exactly this,
 * but it defines @c __cpuid as a five-parameter MACRO, and on Windows that
 * clashes with the @c __cpuid declaration in MSVC's `<intrin.h>`, which comes
 * in by itself through any standard-library header.  Checked: with Clang
 * targeting MSVC, including both breaks the build with "too few arguments
 * provided to function-like macro".  A library that wants to be includable
 * anywhere cannot carry that mine.
 *
 * @par Threads
 * Both are safe from any thread: they read processor state and touch no memory.
 *
 * \~spanish
 * Aqui NO hay ninguna decision sobre capacidades: esto contesta lo que dice el
 * procesador, y quien interpreta esas banderas es @c util/mem/x86/x86_cpu.h.
 * Estan separadas porque son dos cosas distintas -- una es una instruccion, la
 * otra es una politica -- y porque asi las preguntas de capacidad se leen como
 * preguntas en vez de como cuatro registros y un ensamblador en linea.
 *
 * POR QUE NO SE USA `<cpuid.h>`.  La trae el compilador y hace justo esto,
 * pero define @c __cpuid como una MACRO de cinco parametros, y en Windows eso
 * choca con la declaracion @c __cpuid del `<intrin.h>` de MSVC, que entra sola
 * por cualquier cabecera de la biblioteca estandar.  Comprobado: con Clang
 * apuntando a MSVC, incluir las dos rompe la compilacion con
 * "too few arguments provided to function-like macro".  Una libreria que se
 * quiere poder incluir en cualquier sitio no puede traerse esa mina.
 *
 * @par Hilos
 * Las dos son seguras desde cualquier hilo: leen estado del procesador y no
 * tocan memoria.
 *
 * \~
 */
#ifndef VESTA_UTIL_MEM_X86_CPUID_H
#define VESTA_UTIL_MEM_X86_CPUID_H

#include "util/mem/mem_config.h"

#if defined(VESTA_MEM_ARCH_X86)

/// \~english The four registers @c CPUID returns, in their usual order.
/// \~spanish Los cuatro registros que devuelve @c CPUID, en su orden de
///           siempre.
/// \~
typedef struct {
    /// \~english what the processor leaves in @c eax
    /// \~spanish lo que el procesador deja en @c eax  \~
    uint32_t eax;
    /// \~english what the processor leaves in @c ebx
    /// \~spanish lo que el procesador deja en @c ebx  \~
    uint32_t ebx;
    /// \~english what the processor leaves in @c ecx
    /// \~spanish lo que el procesador deja en @c ecx  \~
    uint32_t ecx;
    /// \~english what the processor leaves in @c edx
    /// \~spanish lo que el procesador deja en @c edx  \~
    uint32_t edx;
} vesta_cpuid_regs;

/**
 * @brief
 * \~english Runs @c CPUID on leaf @p leaf and subleaf @p subleaf.
 * \~spanish Ejecuta @c CPUID sobre la hoja @p leaf y la subhoja @p subleaf.
 * \~
 *
 * \~english
 * The @c ebx dance is not decoration: when compiling position-independent code
 * on 32 bits, @c ebx is the register holding the global offset table and the
 * compiler does NOT allow it as an output.  The @c xchg form is the usual one
 * for that case.  On 64 bits there is no such reservation and it can be asked
 * for directly.
 *
 * @par Threads
 * Safe.  It touches no shared memory.
 *
 * \~spanish
 * El baile de @c ebx no es adorno: al compilar codigo independiente de posicion
 * en 32 bits, @c ebx es el registro que guarda la tabla de desplazamientos
 * globales y el compilador NO deja usarlo como salida.  La forma con @c xchg es
 * la de siempre para ese caso.  En 64 bits no hay tal reserva y se puede pedir
 * directo.
 *
 * @par Hilos
 * Segura.  No toca memoria compartida.
 *
 * \~
 * @param leaf
 * \~english the leaf.  Leaf 0 returns the highest available one in @c eax.
 * \~spanish la hoja.  La 0 devuelve en @c eax la mayor disponible.
 * \~
 * @param subleaf
 * \~english the subleaf, in @c ecx.  Zero for the leaves that do not use it.
 * \~spanish subhoja, en @c ecx.  Cero para las hojas que no la usan.
 * \~
 * @return
 * \~english the four registers exactly as the processor leaves them.
 * \~spanish los cuatro registros tal cual los deja el procesador.
 * \~
 *
 * \~english
 * @code
 *   const vesta_cpuid_regs r = vesta_mem_x86_cpuid(1, 0);
 *   const int has_sse42 = (r.ecx & (1u << 20)) != 0;
 * @endcode
 *
 * \~spanish
 * @code
 *   const vesta_cpuid_regs r = vesta_mem_x86_cpuid(1, 0);
 *   const int tiene_sse42 = (r.ecx & (1u << 20)) != 0;
 * @endcode
 *
 * \~
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
 * @brief
 * \~english The highest @c CPUID leaf this processor understands.
 * \~spanish La mayor hoja de @c CPUID que entiende este procesador.
 * \~
 *
 * \~english
 * It has to be asked BEFORE using a high leaf: asking for one that does not
 * exist gives no error, it returns the last one that does, and its bits would
 * be read as if they were those of the one being asked for.
 *
 * @par Threads
 * Safe.
 *
 * \~spanish
 * Hay que preguntarlo ANTES de usar una hoja alta: pedir una que no existe no
 * da error, devuelve la ultima que si existe, y sus bits se leerian como si
 * fueran los de la que se pedia.
 *
 * @par Hilos
 * Segura.
 *
 * \~
 * @return
 * \~english the number of the last basic leaf available.
 * \~spanish el numero de la ultima hoja basica disponible.
 * \~
 *
 * \~english
 * @code
 *   if (vesta_mem_x86_cpuid_max() >= 7) { ... }
 * @endcode
 *
 * \~spanish
 * @code
 *   if (vesta_mem_x86_cpuid_max() >= 7) { ... }
 * @endcode
 *
 * \~
 */
VESTA_MEM_ALWAYS_INLINE uint32_t
vesta_mem_x86_cpuid_max(void) VESTA_MEM_NOEXCEPT {
    return vesta_mem_x86_cpuid(0, 0).eax;
}

/**
 * @brief
 * \~english Reads an extended control register with @c XGETBV.
 * \~spanish Lee un registro de control extendido con @c XGETBV.
 * \~
 *
 * \~english
 * It is what answers which state the operating system SAVES on a task switch,
 * which is a different question from what the CPU can do.
 *
 * @warning It can only be run if leaf 1 reports OSXSAVE (bit 27 of @c ecx).
 *          Without that, the instruction raises an invalid-instruction
 *          exception.  The caller checks first.
 *
 * @par Threads
 * Safe.
 *
 * \~spanish
 * Es lo que contesta que estado GUARDA el sistema operativo al cambiar de
 * tarea, que es una pregunta distinta de que sabe hacer la CPU.
 *
 * @warning Solo se puede ejecutar si la hoja 1 trae OSXSAVE (bit 27 de @c ecx).
 *          Sin eso, la instruccion provoca una excepcion de instruccion no
 *          valida.  Quien llama comprueba primero.
 *
 * @par Hilos
 * Segura.
 *
 * \~
 * @param index
 * \~english which register.  0 is XCR0, the extended-state one.
 * \~spanish que registro.  El 0 es XCR0, el del estado extendido.
 * \~
 * @return
 * \~english the register's 64 bits (@c edx:eax).
 * \~spanish los 64 bits del registro (@c edx:eax).
 * \~
 *
 * \~english
 * @code
 *   // bits 1 and 2: the system saves XMM and YMM
 *   const int ymm_ok = (vesta_mem_x86_xgetbv(0) & 0x6u) == 0x6u;
 * @endcode
 *
 * \~spanish
 * @code
 *   // bits 1 y 2: el sistema guarda XMM e YMM
 *   const int ymm_ok = (vesta_mem_x86_xgetbv(0) & 0x6u) == 0x6u;
 * @endcode
 *
 * \~
 */
VESTA_MEM_ALWAYS_INLINE uint64_t
vesta_mem_x86_xgetbv(uint32_t index) VESTA_MEM_NOEXCEPT {
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(index));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

#endif // VESTA_MEM_ARCH_X86

#endif // VESTA_UTIL_MEM_X86_CPUID_H

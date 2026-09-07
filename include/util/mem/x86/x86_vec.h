/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/mem/x86/x86_vec.h
 * @brief
 * \~english x86 vector types.  Every micro-ISA in here shares them.
 * \~spanish Tipos vectoriales de x86.  Los comparten todas las micro-ISA de
 *          aqui.
 * \~
 *
 * \~english
 * They are GCC/Clang vector types (@c __attribute__((vector_size))), not Intel
 * intrinsics.  On purpose: the intrinsic ties the file to one particular ISA
 * and forces `<immintrin.h>` to be included -- which is not freestanding either
 * -- whereas the vector type lets each function's @c target attribute decide
 * which instructions it materialises as.
 *
 * \~spanish
 * Son tipos vectoriales de GCC/Clang (@c __attribute__((vector_size))), no
 * intrinsecos de Intel.  A proposito: el intrinseco ata el fichero a una ISA
 * concreta y obliga a incluir `<immintrin.h>` -- que ademas no es freestanding
 * --, mientras que el tipo vectorial deja que sea el atributo @c target de cada
 * funcion quien decida con que instrucciones se materializa.
 *
 * \~
 */
#ifndef VESTA_UTIL_MEM_X86_VEC_H
#define VESTA_UTIL_MEM_X86_VEC_H

#include "util/mem/mem_config.h"

#if defined(VESTA_MEM_ARCH_X86)

/* @c aligned(1) es IMPRESCINDIBLE, no un detalle: sin el, un tipo vectorial
 * tiene alineacion NATURAL (16/32 bytes) y el compilador emite el movimiento
 * ALINEADO (`movdqa`), que FALLA con violacion de segmento en cuanto la
 * direccion no lo esta -- y aqui las direcciones vienen de quien llame, sin
 * garantia ninguna.  Con @c aligned(1) se emiten los no alineados
 * (`movdqu`/`vmovdqu`), que en las CPU modernas cuestan lo mismo cuando el dato
 * SI esta alineado.
 *
 * @c may_alias es lo que hace legal leer y escribir a traves de estos tipos
 * sobre un @c uint8_t* cualquiera sin romper el aliasing estricto. */

/// \~english A 16-byte block.  SSE2, guaranteed on every x86-64.
/// \~spanish Bloque de 16 bytes.  SSE2, garantizado en todo x86-64.
/// \~
typedef uint8_t vesta_v16
    __attribute__((vector_size(16), may_alias, aligned(1)));

#if defined(VESTA_MEM_ARCH_X86_64)
/// \~english A 32-byte block.  Only a function compiled with AVX2 materialises
///           it.
/// \~spanish Bloque de 32 bytes.  Solo lo materializa una funcion compilada con
///           AVX2.
/// \~
typedef uint8_t vesta_v32
    __attribute__((vector_size(32), may_alias, aligned(1)));

/// \~english The same block with the element type Intel's builtins ask for.
/// \~spanish El mismo bloque con el tipo de elemento que piden los builtins de
///           Intel.
/// \~
typedef char vesta_v32c __attribute__((vector_size(32)));

/* Las variantes ALINEADAS: sin `aligned(1)`, o sea con la alineacion natural,
 * que es lo que hace que el compilador emita `movaps`/`vmovdqa`.  Escribir con
 * ellas en una direccion no alineada es una violacion de segmento, asi que solo
 * se usan tras haber alineado el destino a proposito. */
/// \~english The 16-byte block with its NATURAL alignment.
/// \~spanish El bloque de 16 bytes con su alineacion NATURAL.
/// \~
typedef uint8_t vesta_v16a __attribute__((vector_size(16), may_alias));
/// \~english The 32-byte block with its NATURAL alignment.
/// \~spanish El bloque de 32 bytes con su alineacion NATURAL.
/// \~
typedef uint8_t vesta_v32a __attribute__((vector_size(32), may_alias));

/**
 * @def VESTA_MEM_STORE16A
 * @def VESTA_MEM_STORE32A
 * @brief
 * \~english Writing 16 or 32 bytes at an ALREADY ALIGNED address.
 * \~spanish Escribir 16 o 32 bytes en una direccion YA ALINEADA.
 * \~
 *
 * \~english
 * WHY IT PAYS TO ALIGN BEFORE THE LOOP.  An unaligned write that crosses a
 * cache line gets split in two inside, and in a loop that is paid on every
 * turn.  It is invisible at the small sizes, where there is no loop, and
 * invisible at the large ones, where memory bandwidth rules; it bites right in
 * the middle.
 *
 * MEASURED with a 1 KiB copy probe, three runs (ns per copy):
 *
 *   unaligned writes, unaligned destination      9.56 - 9.75
 *   aligning the destination first               5.15 - 5.24
 *   destination already aligned to begin with    4.08 - 4.29
 *
 * The number on the first row is exactly what our benchmark gave, and the one
 * on the second, exactly glibc's.  It is the technique this code was missing,
 * and it came out of DISASSEMBLING glibc, not out of guessing: it keeps the
 * head and the tail in registers, rounds the destination up to the next
 * boundary, and the loop writes aligned with @c movaps while still reading
 * unaligned.
 *
 * The builtin detour is not needed here: GCC only splits UNALIGNED writes, so a
 * plain assignment already emits the right instruction.
 *
 * \~spanish
 * POR QUE MERECE LA PENA ALINEAR ANTES DEL BUCLE.  Una escritura sin alinear
 * que cruza una linea de cache se parte en dos por dentro, y en un bucle eso se
 * paga en cada vuelta.  Es invisible en los tamanos pequenos, donde no hay
 * bucle, e invisible en los grandes, donde manda el ancho de banda de memoria;
 * muerde justo en medio.
 *
 * MEDIDO con un probe de copia de 1 KiB, tres corridas (ns por copia):
 *
 *   escrituras sin alinear, destino desalineado   9,56 - 9,75
 *   alineando el destino primero                  5,15 - 5,24
 *   destino ya alineado de entrada                4,08 - 4,29
 *
 * El numero de la primera fila es exactamente el que daba nuestro banco, y el
 * de la segunda, exactamente el de glibc.  Es la tecnica que le faltaba a este
 * codigo, y salio de DESENSAMBLAR glibc, no de suponer: guarda la cabeza y la
 * cola en registros, redondea el destino al siguiente limite, y el bucle
 * escribe alineado con @c movaps mientras sigue leyendo sin alinear.
 *
 * Aqui no hace falta el rodeo del builtin: GCC solo parte las escrituras SIN
 * alinear, asi que una asignacion normal ya emite la instruccion buena.
 *
 * \~
 */
#define VESTA_MEM_STORE16A(p, v) (*(vesta_v16a *)(p) = (v))
#define VESTA_MEM_STORE32A(p, v) (*(vesta_v32a *)(p) = (v))

/**
 * @brief
 * \~english How many bytes have to be copied unaligned for @p d to end up
 *          aligned to @p a.
 * \~spanish Cuantos bytes hay que copiar sin alinear para que @p d quede
 *          alineado a @p a.
 * \~
 *
 * \~english
 * @par Threads
 * Safe.  It only computes.
 *
 * \~spanish
 * @par Hilos
 * Segura.  Solo calcula.
 *
 * \~
 * @param d
 * \~english the destination address.
 * \~spanish direccion de destino.
 * \~
 * @param a
 * \~english the alignment wanted, a power of two.
 * \~spanish alineacion buscada, potencia de dos.
 * \~
 * @return
 * \~english from 0 to @p a - 1.  Zero when it was already aligned.
 * \~spanish de 0 a @p a - 1.  Cero si ya estaba alineada.
 * \~
 *
 * \~english
 * @code
 *   const size_t head = vesta_mem_x86_head_to_align(d, 32);
 * @endcode
 *
 * \~spanish
 * @code
 *   const size_t head = vesta_mem_x86_head_to_align(d, 32);
 * @endcode
 *
 * \~
 */
VESTA_MEM_ALWAYS_INLINE size_t
vesta_mem_x86_head_to_align(const void *d, size_t a) VESTA_MEM_NOEXCEPT {
    const size_t off = (size_t)(uintptr_t)d & (a - 1u);
    return off != 0 ? a - off : 0;
}

/**
 * @def VESTA_MEM_LOAD32
 * @def VESTA_MEM_STORE32
 * @brief
 * \~english Reading and writing 32 unaligned bytes, in ONE instruction.
 * \~spanish Leer y escribir 32 bytes sin alinear, en UNA instruccion.
 * \~
 *
 * \~english
 * Needing a macro for this is not a whim.  DISASSEMBLED: GCC splits every
 * unaligned 32-byte write into two of 16 -- @c vmovdqu @c %xmm0 plus
 * @c vextracti128 -- so a loop that should emit eight writes emits sixteen
 * instructions.  It is @c -mavx256-split-unaligned-store, a heuristic that
 * comes from Sandy Bridge, where splitting WAS faster.
 *
 * And the place it is applied is exactly the wrong one: this routine only runs
 * if the CPU has AVX2, that is Haswell onwards, where the wide write is already
 * the right one.  It is tuning for a microarchitecture that CANNOT run this
 * code.
 *
 * Measured on a 4 KiB fill with GCC 10 and @c -mtune=generic: 34.45 ns
 * splitting against 15.40 not splitting.  And it cannot be turned off from the
 * header -- neither @c target("no-avx256-split-unaligned-store"), which does
 * not exist as an attribute option, nor @c target("tune=skylake"), which
 * compiles but does not change the decision.  The only thing that dodges it is
 * asking for the instruction by name.
 *
 * On Clang it is not needed: disassembled too, it splits nothing.  It gets the
 * plain assignment, which is what its optimiser understands best.
 *
 * \~spanish
 * Que haga falta un macro para esto no es capricho.  DESENSAMBLADO: GCC parte
 * cada escritura de 32 bytes sin alinear en dos de 16 --
 * @c vmovdqu @c %xmm0 mas @c vextracti128 --, asi que un bucle que deberia
 * emitir ocho escrituras emite dieciseis instrucciones.  Es
 * @c -mavx256-split-unaligned-store, una heuristica que viene de Sandy Bridge,
 * donde partir SI era mas rapido.
 *
 * Y el sitio donde se aplica es justo el equivocado: esta rutina solo se
 * ejecuta si la CPU tiene AVX2, o sea Haswell en adelante, donde la escritura
 * ancha ya es la buena.  Se esta afinando para una microarquitectura que NO
 * puede ejecutar este codigo.
 *
 * Medido en un relleno de 4 KiB con GCC 10 y @c -mtune=generic: 34,45 ns
 * partiendo contra 15,40 sin partir.  Y no se puede apagar desde la cabecera --
 * ni @c target("no-avx256-split-unaligned-store"), que no existe como opcion
 * del atributo, ni @c target("tune=skylake"), que compila pero no cambia la
 * decision --.  Lo unico que la esquiva es pedir la instruccion por su nombre.
 *
 * En Clang no hace falta: desensamblado tambien, no parte nada.  Se le deja la
 * asignacion normal, que es lo que mejor entiende su optimizador.
 *
 * \~
 */
#if defined(VESTA_MEM_COMPILER_GCC)
#define VESTA_MEM_LOAD32(p)                                                   \
    ((vesta_v32)__builtin_ia32_loaddqu256((const char *)(p)))
#define VESTA_MEM_STORE32(p, v)                                               \
    __builtin_ia32_storedqu256((char *)(p), (vesta_v32c)(v))
#else
#define VESTA_MEM_LOAD32(p) (*(const vesta_v32 *)(p))
#define VESTA_MEM_STORE32(p, v) (*(vesta_v32 *)(p) = (v))
#endif
#endif

/**
 * @brief
 * \~english One byte repeated across the 16 lanes, IN A REGISTER.
 * \~spanish Un byte repetido en los 16 carriles, EN REGISTRO.
 * \~
 *
 * \~english
 * The form matters, and much more than it looks.  Writing the pattern to memory
 * and re-reading it as a vector -- which is what comes out of building it with
 * @c __builtin_memcpy -- makes GCC leave the RELOAD INSIDE the loop:
 *
 *     mov    %rsi,-0x18(%rsp)          <- it writes it
 *     ...
 *     movdqa -0x18(%rsp),%xmm0         <- and re-reads it EVERY TURN
 *
 * That is one store-to-load forward per iteration, and it cost 5.2 ns where
 * this version costs 1.5 -- 3.4x, measured in @c bench_memset.  The vector
 * addition with a scalar is the form both compilers turn into the real
 * broadcast (@c movd + @c punpcklbw + @c punpcklwd + @c pshufd), outside the
 * loop and without touching memory.
 *
 * @par Threads
 * Safe.  It only computes.
 *
 * \~spanish
 * La forma importa, y mucho mas de lo que parece.  Escribir el patron a memoria
 * y releerlo como vector -- que es lo que sale de armarlo con
 * @c __builtin_memcpy -- hace que GCC deje la RECARGA DENTRO del bucle:
 *
 *     mov    %rsi,-0x18(%rsp)          <- lo escribe
 *     ...
 *     movdqa -0x18(%rsp),%xmm0         <- y lo relee CADA VUELTA
 *
 * Eso es un reenvio de almacen a carga por iteracion, y costaba 5,2 ns donde
 * esta version cuesta 1,5 -- 3,4x, medido en @c bench_memset --.  La suma
 * vectorial con un escalar es la forma que los dos compiladores convierten en
 * la difusion de verdad (@c movd + @c punpcklbw + @c punpcklwd + @c pshufd),
 * fuera del bucle y sin tocar memoria.
 *
 * @par Hilos
 * Segura.  Solo calcula.
 *
 * \~
 * @param v
 * \~english the byte to repeat.
 * \~spanish byte a repetir.
 * \~
 * @return
 * \~english the vector with @p v in all 16 lanes.
 * \~spanish el vector con @p v en los 16 carriles.
 * \~
 *
 * \~english
 * @code
 *   const vesta_v16 pat = vesta_mem_x86_splat16(0xFF);
 * @endcode
 *
 * \~spanish
 * @code
 *   const vesta_v16 pat = vesta_mem_x86_splat16(0xFF);
 * @endcode
 *
 * \~
 */
VESTA_MEM_ALWAYS_INLINE vesta_v16
vesta_mem_x86_splat16(uint8_t v) VESTA_MEM_NOEXCEPT {
    const vesta_v16 zero = {0};
    return zero + v;
}

#if defined(VESTA_MEM_ARCH_X86_64)
/**
 * @brief
 * \~english The same across the 32 lanes.  See @c vesta_mem_x86_splat16.
 * \~spanish Lo mismo en los 32 carriles.  Ver @c vesta_mem_x86_splat16.
 * \~
 *
 * \~english
 * @par Threads
 * Safe.
 *
 * \~spanish
 * @par Hilos
 * Segura.
 *
 * \~
 * @param v
 * \~english the byte to repeat.
 * \~spanish byte a repetir.
 * \~
 * @return
 * \~english the vector with @p v in all 32 lanes.
 * \~spanish el vector con @p v en los 32 carriles.
 * \~
 *
 * \~english
 * @code
 *   const vesta_v32 pat = vesta_mem_x86_splat32(0);
 * @endcode
 *
 * \~spanish
 * @code
 *   const vesta_v32 pat = vesta_mem_x86_splat32(0);
 * @endcode
 *
 * \~
 */
VESTA_MEM_AVX2_FN vesta_v32
vesta_mem_x86_splat32(uint8_t v) VESTA_MEM_NOEXCEPT {
    const vesta_v32 zero = {0};
    return zero + v;
}
#endif

#endif // VESTA_MEM_ARCH_X86

#endif // VESTA_UTIL_MEM_X86_VEC_H

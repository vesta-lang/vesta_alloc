/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/mem/x86/x86_vec.h
 * @brief Tipos vectoriales de x86.  Los comparten todas las micro-ISA de aqui.
 *
 * Son tipos vectoriales de GCC/Clang (@c __attribute__((vector_size))), no
 * intrinsecos de Intel.  A proposito: el intrinseco ata el fichero a una ISA
 * concreta y obliga a incluir @c <immintrin.h> -- que ademas no es freestanding
 * --, mientras que el tipo vectorial deja que sea el atributo @c target de cada
 * funcion quien decida con que instrucciones se materializa.
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

/// Bloque de 16 bytes.  SSE2, garantizado en todo x86-64.
typedef uint8_t vesta_v16
    __attribute__((vector_size(16), may_alias, aligned(1)));

#if defined(VESTA_MEM_ARCH_X86_64)
/// Bloque de 32 bytes.  Solo lo materializa una funcion compilada con AVX2.
typedef uint8_t vesta_v32
    __attribute__((vector_size(32), may_alias, aligned(1)));

/// El mismo bloque con el tipo de elemento que piden los builtins de Intel.
typedef char vesta_v32c __attribute__((vector_size(32)));

/* Las variantes ALINEADAS: sin `aligned(1)`, o sea con la alineacion natural,
 * que es lo que hace que el compilador emita `movaps`/`vmovdqa`.  Escribir con
 * ellas en una direccion no alineada es una violacion de segmento, asi que solo
 * se usan tras haber alineado el destino a proposito. */
typedef uint8_t vesta_v16a __attribute__((vector_size(16), may_alias));
typedef uint8_t vesta_v32a __attribute__((vector_size(32), may_alias));

/**
 * @def VESTA_MEM_STORE16A
 * @def VESTA_MEM_STORE32A
 * @brief Escribir 16 o 32 bytes en una direccion YA ALINEADA.
 *
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
 */
#define VESTA_MEM_STORE16A(p, v) (*(vesta_v16a *)(p) = (v))
#define VESTA_MEM_STORE32A(p, v) (*(vesta_v32a *)(p) = (v))

/**
 * @brief Cuantos bytes hay que copiar sin alinear para que @p d quede alineado
 *        a @p a.
 *
 * @param d Direccion de destino.
 * @param a Alineacion buscada, potencia de dos.
 * @return De 0 a @p a - 1.  Cero si ya estaba alineada.
 *
 * @par Hilos
 * Segura.  Solo calcula.
 *
 * @code
 *   const size_t head = vesta_mem_x86_head_to_align(d, 32);
 * @endcode
 */
VESTA_MEM_ALWAYS_INLINE size_t
vesta_mem_x86_head_to_align(const void *d, size_t a) VESTA_MEM_NOEXCEPT {
    const size_t off = (size_t)(uintptr_t)d & (a - 1u);
    return off != 0 ? a - off : 0;
}

/**
 * @def VESTA_MEM_LOAD32
 * @def VESTA_MEM_STORE32
 * @brief Leer y escribir 32 bytes sin alinear, en UNA instruccion.
 *
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
 * @brief Un byte repetido en los 16 carriles, EN REGISTRO.
 *
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
 * @param v Byte a repetir.
 * @return El vector con @p v en los 16 carriles.
 *
 * @par Hilos
 * Segura.  Solo calcula.
 *
 * @code
 *   const vesta_v16 pat = vesta_mem_x86_splat16(0xFF);
 * @endcode
 */
VESTA_MEM_ALWAYS_INLINE vesta_v16
vesta_mem_x86_splat16(uint8_t v) VESTA_MEM_NOEXCEPT {
    const vesta_v16 zero = {0};
    return zero + v;
}

#if defined(VESTA_MEM_ARCH_X86_64)
/**
 * @brief Lo mismo en los 32 carriles.  Ver @c vesta_mem_x86_splat16.
 *
 * @param v Byte a repetir.
 * @return El vector con @p v en los 32 carriles.
 *
 * @par Hilos
 * Segura.
 *
 * @code
 *   const vesta_v32 pat = vesta_mem_x86_splat32(0);
 * @endcode
 */
VESTA_MEM_AVX2_FN vesta_v32
vesta_mem_x86_splat32(uint8_t v) VESTA_MEM_NOEXCEPT {
    const vesta_v32 zero = {0};
    return zero + v;
}
#endif

#endif // VESTA_MEM_ARCH_X86

#endif // VESTA_UTIL_MEM_X86_VEC_H

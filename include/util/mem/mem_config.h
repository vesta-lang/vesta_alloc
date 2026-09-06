/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/mem/mem_config.h
 * @brief Que caminos de memoria hay compilados en ESTA construccion, y como
 *        esta repartido todo lo demas.
 *
 * COMO ESTA ORGANIZADO.  Una carpeta por ARQUITECTURA, y dentro un fichero por
 * MICRO-ISA y operacion.  No es orden por el orden: mezclar SSE2, AVX2 y lo que
 * venga en un solo fichero hace que cada extension nueva lo ensucie mas, y que
 * anadir ARM obligue a tocar el codigo de x86.
 *
 *   util/mem/mem_config.h        esto: que hay compilado
 *   util/mem/mem_inline.h        lo que NO depende de la ISA (menos de 16 bytes)
 *   util/mem/x86/x86_vec.h       tipos vectoriales de x86
 *   util/mem/x86/x86_cpuid.h     CPUID y XGETBV, envueltas y nada mas
 *   util/mem/x86/x86_cpu.h       que sabe hacer esta CPU (interpreta lo de
 *                                arriba: preguntar y decidir van aparte)
 *   util/mem/x86/sse2_memcpy.h   vesta_mem_sse2_copy
 *   util/mem/x86/sse2_memset.h   vesta_mem_sse2_fill
 *   util/mem/x86/avx2_memcpy.h   vesta_mem_avx2_copy
 *   util/mem/x86/avx2_memset.h   vesta_mem_avx2_fill
 *   util/mem/generic/scalar_*.h  vesta_mem_scalar_copy / _fill
 *   util/vesta_memcpy.h          el despacho, y lo unico que se incluye fuera
 *   util/vesta_memset.h          idem
 *
 * CADA MICRO-ISA EXPONE LO MISMO: @c copy y @c fill con la misma firma, bajo su
 * propio prefijo.  Esa uniformidad es la que hace barato anadir una
 * arquitectura -- para NEON serian @c util/mem/arm/neon_memcpy.h con
 * @c vesta_mem_neon_copy, un @c util/mem/arm/arm_cpu.h que conteste si la hay, y
 * UNA rama mas en el despachador --.  Ni el codigo de x86 ni el camino de los
 * tamanos pequenos se tocan.
 *
 * ESTO ES C, NO C++.  Todas las cabeceras de memoria compilan con un compilador
 * de C y con uno de C++, y no es un capricho: para que una dependencia en C
 * pueda usarlas SIN PAGAR UNA LLAMADA, su compilador tiene que ver el cuerpo.
 * Una capa en C++ con un envoltorio en C daria a las dependencias justo lo que
 * se esta intentando quitar.  De ahi que no haya espacios de nombres, ni
 * plantillas, ni @c <atomic>: prefijos, @c static @c inline y los atomicos del
 * compilador, que existen en los dos lenguajes.  Encima de eso,
 * @c util/vesta_memcpy.h anade los envoltorios @c util::vesta_memcpy para que
 * en C++ se lea como C++.
 *
 * LOS DOS EJES QUE DECIDEN QUE SE COMPILA, que son distintos y conviene no
 * confundir:
 *
 *   - @c VESTA_ALLOC_FREESTANDING -- si se puede prescindir de la biblioteca C.
 *     Depende del COMPILADOR (hacen falta tipos vectoriales y @c target por
 *     funcion, que son extensiones de GCC/Clang), no de la maquina.
 *   - @c VESTA_MEM_ARCH_* -- que familia de caminos existe.  Depende del
 *     objetivo.
 *
 * Que un camino este COMPILADO no quiere decir que se USE: si la CPU tiene AVX2
 * lo decide @c vesta_mem_x86_has_avx2() en ejecucion.
 */
#ifndef VESTA_UTIL_MEM_CONFIG_H
#define VESTA_UTIL_MEM_CONFIG_H

/* Las de C y no las de C++ (`<cstdint>`), porque estas cabeceras las compila
 * tambien un compilador de C.  Ademas son las dos unicas que un entorno
 * freestanding tiene garantizadas: las trae el COMPILADOR, no la libreria. */
#include <stddef.h>
#include <stdint.h>

/* Sin las extensiones de GCC/Clang no hay camino propio posible, y entonces
 * -- y SOLO entonces -- se llama a la biblioteca C.  Se deja anular desde
 * fuera para poder medir la comparacion sin tocar el codigo. */
#if !defined(VESTA_ALLOC_FREESTANDING)
#if defined(__GNUC__) || defined(__clang__)
#define VESTA_ALLOC_FREESTANDING 1
#else
#define VESTA_ALLOC_FREESTANDING 0
#endif
#endif

#if !VESTA_ALLOC_FREESTANDING
#include <string.h> // el respaldo, y el unico sitio donde entra
#endif

#if VESTA_ALLOC_FREESTANDING
#if defined(__x86_64__) || defined(_M_X64)
#define VESTA_MEM_ARCH_X86 1
#define VESTA_MEM_ARCH_X86_64 1 ///< 64 bits: ademas del base, se compila AVX2
#elif defined(__i386__) || defined(_M_IX86)
#define VESTA_MEM_ARCH_X86 1
#endif
#endif

/* --- Cuando la micro-ISA ya viene FIJADA de la linea de ordenes ------------
 *
 * El despacho en ejecucion existe porque la linea base del proyecto es
 * `-march=x86-64` y no se sabe donde va a correr el binario.  Pero si a alguien
 * le compilan con `-mavx2` o con `-march=native`, esa duda YA NO EXISTE: el
 * ejecutable no arranca en una maquina sin AVX2, asi que preguntarselo en cada
 * llamada es una lectura y una rama que sobran -- y en los tamanos pequenos,
 * donde la copia entera son cuatro instrucciones, eso se nota.
 *
 * Con la micro-ISA fija pasan las dos cosas buenas a la vez: la pregunta se
 * pliega a una constante y desaparece con la rama, y la rutina ancha deja de
 * necesitar el atributo `target`, con lo que SE PUEDE METER EN LINEA -- que es
 * lo que la separaba del camino base --.  Es decir, `vesta_memcpy_inline`
 * tambien pasa a usar AVX2.
 *
 * Se deja forzar a mano (`-DVESTA_MEM_TARGET_AVX2=1`) para quien sepa donde va
 * a ejecutar y no quiera compilar el resto del programa con `-mavx2`. */
#if !defined(VESTA_MEM_TARGET_AVX2) && defined(__AVX2__)
#define VESTA_MEM_TARGET_AVX2 1
#endif

/**
 * @def VESTA_MEM_AVX2_MIN
 * @brief A partir de cuantos bytes compensa SALIR a la rutina de AVX2.
 *
 * Que exista este umbral es consecuencia de una limitacion, no de una eleccion:
 * una funcion con @c target("avx2") no se puede meter en linea en otra que no
 * lo lleve, asi que usarla cuesta una LLAMADA.  Y por debajo de cierto tamano
 * la llamada cuesta mas que lo que se gana moviendo de 32 en 32.
 *
 * Se ve en el banco: la copia de 64 bytes va a 0,58 ns y la de 128 saltaba a
 * 2,10, con la mitad del trabajo por byte.  Ese salto no era trabajo, era la
 * llamada.  Por debajo del umbral se usa el bucle base, que se mete en linea.
 *
 * MEDIDO barriendo el umbral, tres corridas por punto (ns, GCC 15 / Linux):
 *
 *   umbral    128 B    256 B     1 KiB
 *       64     2,15     2,70      9,67
 *      256     1,77     2,71      9,97
 *      512     1,73     2,54      9,82
 *     1024     1,83     2,64      9,58
 *
 * Y de ahi sale algo que conviene no perder de vista: **a 1 KiB da IGUAL** --
 * el bucle de 32 bytes rinde lo mismo que el de 16 --, o sea que ahi el limite
 * no es el ancho del vector.  Es la razon de que sigamos a 0,55x de glibc en
 * esa banda, y no se arregla ensanchando.
 *
 * Con @c -mavx2 esto no pinta nada: alli la rutina ancha tambien se inlinea.
 */
#if !defined(VESTA_MEM_AVX2_MIN)
#define VESTA_MEM_AVX2_MIN 512
#endif

/**
 * @def VESTA_MEM_AVX2_STEP
 * @brief Cuantos bytes mueve una vuelta del bucle ancho: 128 (cuatro
 *        movimientos) o 256 (ocho).
 *
 * No es afinado a ojo, sale de un perfil: VTune dice que el bucle NO esta
 * atascado -- 0,267 ciclos por instruccion, casi cuatro instrucciones por ciclo
 * -- y que aun asi retira ~133 instrucciones por cada KiB copiado, cuando el
 * trabajo son ~70 movimientos.  Lo que sobra es la contabilidad de la vuelta
 * (avanzar dos punteros, restar, comparar, saltar), y doblar el escalon la
 * reparte entre el doble de bytes.
 */
#if !defined(VESTA_MEM_AVX2_STEP)
#define VESTA_MEM_AVX2_STEP 256
#endif

/* --- Lo que hace falta para escribir una vez y compilar en los dos ---------
 *
 * `static inline` y no `inline` a secas: en C99 un `inline` sin `extern` en
 * otra unidad no genera simbolo y el enlace falla, mientras que `static inline`
 * significa lo mismo en los dos lenguajes -- una copia por unidad, que es justo
 * lo que se quiere de algo pensado para meterse en linea. */

/// Se mete en linea SIEMPRE.  Es lo que hace que no haya llamada.
#define VESTA_MEM_ALWAYS_INLINE __attribute__((always_inline)) static inline

/// Se puede meter en linea, pero no se obliga.  Para lo que lleva @c target.
#define VESTA_MEM_INLINE static inline

/**
 * @def VESTA_MEM_NOINLINE
 * @brief Una funcion de verdad, que NO se mete en linea.
 *
 * Sin @c inline a proposito: @c noinline junto a @c inline es una
 * contradiccion y el compilador avisa de ella.  Y con @c unused porque al
 * vivir en una cabecera va a haber unidades que no la llamen, y ahi un
 * @c static sin usar tambien avisa -- dos avisos que taparian los de verdad.
 */
#define VESTA_MEM_NOINLINE __attribute__((noinline, unused)) static

/**
 * @def VESTA_MEM_AVX2_FN
 * @brief Como se declara una rutina de AVX2, que depende de si la micro-ISA
 *        vino fijada al compilar.
 *
 * Con @c -mavx2 (o @c -march=native en una maquina que lo tenga) sobra el
 * atributo @c target -- el fichero entero ya se compila asi --, y quitarlo es
 * lo que permite METERLA EN LINEA: una funcion con @c target no se puede
 * inlinear en otra que no lo lleve, y esa es toda la razon de que existan dos
 * entradas publicas.  Sin @c -mavx2 se queda como estaba: compilada aparte y
 * alcanzada por una llamada, tras preguntar a la CPU.
 */
#if defined(VESTA_MEM_TARGET_AVX2)
#define VESTA_MEM_AVX2_FN VESTA_MEM_ALWAYS_INLINE
#else
#define VESTA_MEM_AVX2_FN __attribute__((target("avx2"))) VESTA_MEM_INLINE
#endif

#ifdef __cplusplus
#define VESTA_MEM_NOEXCEPT noexcept
#else
#define VESTA_MEM_NOEXCEPT
#endif

/* --- Y lo que hay que darle a cada compilador ------------------------------
 *
 * GCC y Clang no fallan en lo mismo, asi que no se les puede dar lo mismo.  Las
 * dos cosas de aqui abajo salen de MEDIR el codigo generado, no de suponer, y
 * cada una arregla un problema distinto. */
#if defined(__clang__)
#define VESTA_MEM_COMPILER_CLANG 1
#elif defined(__GNUC__)
#define VESTA_MEM_COMPILER_GCC 1
#endif

/**
 * @def VESTA_MEM_KEEP_LOOP
 * @brief Impide que el compilador reconozca el bucle y lo sustituya POR una
 *        llamada a @c memset / @c memcpy.
 *
 * Suena absurdo y no lo es: los optimizadores traen un pase que detecta "este
 * bucle rellena una region" y lo cambia por la llamada a la biblioteca, porque
 * normalmente eso es una mejora.  Aqui es lo contrario -- ESTO ES la
 * implementacion --, y el resultado seria una recursion disfrazada y, sobre
 * todo, la dependencia de libc que toda esta carpeta existe para no tener.
 *
 * COMPROBADO, no supuesto: con @c vesta_memset_inline(p, 0, n), Clang deja
 * @c memset sin resolver en el objeto y GCC no.  Basta con que el puntero sea
 * OPACO una vez por vuelta para que el pase no pueda reconocer el patron; el
 * asm esta vacio, asi que no se emite ninguna instruccion.
 *
 * En GCC no se pone porque no hace falta: su pase equivalente
 * (@c -ftree-loop-distribute-patterns) solo mordio en los bucles de BYTE, que
 * ya no existen aqui -- ver @c util/mem/mem_inline.h.
 */
#if defined(VESTA_MEM_COMPILER_CLANG)
#define VESTA_MEM_KEEP_LOOP(p) __asm__ volatile("" : "+r"(p))
#else
#define VESTA_MEM_KEEP_LOOP(p) ((void)0)
#endif

/**
 * @def VESTA_MEM_NO_UNROLL
 * @brief Prohibe al compilador desenrollar el bucle que viene detras.
 *
 * EL DESENROLLADO YA ESTA HECHO, y a mano: cada vuelta mueve 64, 128 o 256
 * bytes, y ese numero salio de medir.  Que el compilador desenrolle ENCIMA es
 * pasarse por encima de una decision que ya se tomo con datos.
 *
 * Y no es teorico.  DESENSAMBLADO: cuando el tamano llega como constante -- que
 * es lo normal desde la interfaz con tipo -- Clang se lleva la cuenta de
 * vueltas al compilador y despliega el bucle entero.  Un relleno de 4 KiB pasa
 * de 137 instrucciones a 407.  Cambia unas pocas ramas por triplicar el
 * codigo, y a ese tamano eso no es una mejora: es presion de cache de
 * instrucciones en cada sitio donde se llame.
 *
 * Con esto, lo que se emite es lo que hay escrito, y el unico efecto de saber
 * el tamano y la alineacion es lo que se pretendia: que sobren comprobaciones,
 * no que aparezca codigo.
 */
#if defined(VESTA_MEM_COMPILER_CLANG)
#define VESTA_MEM_NO_UNROLL _Pragma("clang loop unroll(disable)")
#elif defined(VESTA_MEM_COMPILER_GCC)
#define VESTA_MEM_NO_UNROLL _Pragma("GCC unroll 1")
#else
#define VESTA_MEM_NO_UNROLL
#endif

#endif // VESTA_UTIL_MEM_CONFIG_H

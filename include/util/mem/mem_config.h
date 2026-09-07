/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/mem/mem_config.h
 * @brief
 * \~english Which memory paths are compiled into THIS build, and how everything
 *          else is laid out.
 * \~spanish Que caminos de memoria hay compilados en ESTA construccion, y como
 *          esta repartido todo lo demas.
 * \~
 *
 * \~english
 * HOW IT IS ORGANISED.  One folder per ARCHITECTURE, and inside it one file per
 * MICRO-ISA and operation.  It is not tidiness for its own sake: mixing SSE2,
 * AVX2 and whatever comes next into one file means every new extension makes it
 * dirtier, and that adding ARM forces the x86 code to be touched.
 *
 *   util/mem/mem_config.h        this: what is compiled in
 *   util/mem/mem_inline.h        what does NOT depend on the ISA (under 16 B)
 *   util/mem/x86/x86_vec.h       x86 vector types
 *   util/mem/x86/x86_cpuid.h     CPUID and XGETBV, wrapped and nothing else
 *   util/mem/x86/x86_cpu.h       what this CPU can do (it interprets the above:
 *                                asking and deciding go apart)
 *   util/mem/x86/sse2_memcpy.h   vesta_mem_sse2_copy
 *   util/mem/x86/sse2_memset.h   vesta_mem_sse2_fill
 *   util/mem/x86/avx2_memcpy.h   vesta_mem_avx2_copy
 *   util/mem/x86/avx2_memset.h   vesta_mem_avx2_fill
 *   util/mem/generic/scalar_*.h  vesta_mem_scalar_copy / _fill
 *   util/mem/vesta_memcpy.h      the dispatch, and the only thing included
 *                                from outside
 *   util/mem/vesta_memset.h      likewise
 *
 * EVERY MICRO-ISA EXPOSES THE SAME THING: @c copy and @c fill with the same
 * signature, under its own prefix.  That uniformity is what makes adding an
 * architecture cheap -- for NEON it would be @c util/mem/arm/neon_memcpy.h with
 * @c vesta_mem_neon_copy, an @c util/mem/arm/arm_cpu.h that answers whether it
 * is there, and ONE more branch in the dispatcher.  Neither the x86 code nor
 * the small-size path gets touched.
 *
 * THIS IS C, NOT C++.  Every memory header compiles with a C compiler and with
 * a C++ one, and that is not a whim: for a dependency written in C to use them
 * WITHOUT PAYING A CALL, its compiler has to see the body.  A C++ layer with a
 * C wrapper would hand the dependencies exactly what is being taken away.
 * Hence no namespaces, no templates, no `<atomic>`: prefixes, @c static
 * @c inline and the compiler's atomics, which exist in both languages.  On top
 * of that, @c util/mem/vesta_memcpy.h adds the @c util::vesta_memcpy wrappers
 * so that in C++ it reads like C++.
 *
 * THE TWO AXES THAT DECIDE WHAT GETS COMPILED, which are different and worth
 * not confusing:
 *
 *   - @c VESTA_ALLOC_FREESTANDING -- whether the C library can be done without.
 *     It depends on the COMPILER (vector types and per-function @c target are
 *     needed, which are GCC/Clang extensions), not on the machine.
 *   - @c VESTA_MEM_ARCH_* -- which family of paths exists.  It depends on the
 *     target.
 *
 * A path being COMPILED does not mean it gets USED: whether the CPU has AVX2 is
 * decided by @c vesta_mem_x86_has_avx2() at run time.
 *
 * \~spanish
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
 *   util/mem/vesta_memcpy.h          el despacho, y lo unico que se incluye fuera
 *   util/mem/vesta_memset.h          idem
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
 * plantillas, ni `<atomic>`: prefijos, @c static @c inline y los atomicos del
 * compilador, que existen en los dos lenguajes.  Encima de eso,
 * @c util/mem/vesta_memcpy.h anade los envoltorios @c util::vesta_memcpy para que
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
 *
 * \~
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
/// \~english 64 bits: besides the base, AVX2 gets compiled in
/// \~spanish 64 bits: ademas del base, se compila AVX2
/// \~
#define VESTA_MEM_ARCH_X86_64 1
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
 * @brief
 * \~english From how many bytes on it pays to GO OUT to the AVX2 routine.
 * \~spanish A partir de cuantos bytes compensa SALIR a la rutina de AVX2.
 * \~
 *
 * \~english
 * This threshold existing is the consequence of a limitation, not of a choice:
 * a function with @c target("avx2") cannot be inlined into one that does not
 * carry it, so using it costs a CALL.  And below a certain size the call costs
 * more than is gained by moving 32 at a time.
 *
 * It shows in the benchmark: the 64-byte copy runs at 0.58 ns and the 128-byte
 * one jumped to 2.10, with half the work per byte.  That jump was not work, it
 * was the call.  Below the threshold the base loop is used, which does get
 * inlined.
 *
 * MEASURED by sweeping the threshold, three runs per point (ns, GCC 15 /
 * Linux):
 *
 *   threshold  128 B    256 B     1 KiB
 *       64      2.15     2.70      9.67
 *      256      1.77     2.71      9.97
 *      512      1.73     2.54      9.82
 *     1024      1.83     2.64      9.58
 *
 * And out of that comes something worth not losing sight of: **at 1 KiB it
 * makes NO difference** -- the 32-byte loop performs the same as the 16-byte
 * one -- which means the limit there is not the vector width.  It is the reason
 * we are still at 0.55x of glibc in that band, and it does not get fixed by
 * going wider.
 *
 * With @c -mavx2 this has no part to play: there the wide routine gets inlined
 * too.
 *
 * \~spanish
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
 *
 * \~
 */
#if !defined(VESTA_MEM_AVX2_MIN)
#define VESTA_MEM_AVX2_MIN 512
#endif

/**
 * @def VESTA_MEM_AVX2_STEP
 * @brief
 * \~english How many bytes one turn of the wide loop moves: 128 (four moves) or
 *          256 (eight).
 * \~spanish Cuantos bytes mueve una vuelta del bucle ancho: 128 (cuatro
 *          movimientos) o 256 (ocho).
 * \~
 *
 * \~english
 * It is not tuning by eye, it comes out of a profile: VTune says the loop is
 * NOT stalled -- 0.267 cycles per instruction, almost four instructions per
 * cycle -- and that even so it retires ~133 instructions for every KiB copied,
 * when the work is ~70 moves.  What is left over is the turn's bookkeeping
 * (advancing two pointers, subtracting, comparing, branching), and doubling the
 * step spreads it over twice the bytes.
 *
 * \~spanish
 * No es afinado a ojo, sale de un perfil: VTune dice que el bucle NO esta
 * atascado -- 0,267 ciclos por instruccion, casi cuatro instrucciones por ciclo
 * -- y que aun asi retira ~133 instrucciones por cada KiB copiado, cuando el
 * trabajo son ~70 movimientos.  Lo que sobra es la contabilidad de la vuelta
 * (avanzar dos punteros, restar, comparar, saltar), y doblar el escalon la
 * reparte entre el doble de bytes.
 *
 * \~
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

/// \~english Inlined ALWAYS.  It is what makes there be no call.
/// \~spanish Se mete en linea SIEMPRE.  Es lo que hace que no haya llamada.
/// \~
#define VESTA_MEM_ALWAYS_INLINE __attribute__((always_inline)) static inline

/// \~english It may be inlined, but it is not forced.  For whatever carries
///           @c target.
/// \~spanish Se puede meter en linea, pero no se obliga.  Para lo que lleva
///           @c target.
/// \~
#define VESTA_MEM_INLINE static inline

/**
 * @def VESTA_MEM_NOINLINE
 * @brief
 * \~english A real function, one that is NOT inlined.
 * \~spanish Una funcion de verdad, que NO se mete en linea.
 * \~
 *
 * \~english
 * Without @c inline on purpose: @c noinline next to @c inline is a
 * contradiction and the compiler warns about it.  And with @c unused because,
 * living in a header, there are going to be translation units that never call
 * it, and there an unused @c static warns too -- two warnings that would bury
 * the real ones.
 *
 * \~spanish
 * Sin @c inline a proposito: @c noinline junto a @c inline es una
 * contradiccion y el compilador avisa de ella.  Y con @c unused porque al
 * vivir en una cabecera va a haber unidades que no la llamen, y ahi un
 * @c static sin usar tambien avisa -- dos avisos que taparian los de verdad.
 *
 * \~
 */
#define VESTA_MEM_NOINLINE __attribute__((noinline, unused)) static

/**
 * @def VESTA_MEM_AVX2_FN
 * @brief
 * \~english How an AVX2 routine is declared, which depends on whether the
 *          micro-ISA came fixed at compile time.
 * \~spanish Como se declara una rutina de AVX2, que depende de si la micro-ISA
 *          vino fijada al compilar.
 * \~
 *
 * \~english
 * With @c -mavx2 (or @c -march=native on a machine that has it) the @c target
 * attribute is unnecessary -- the whole file is already compiled that way --
 * and removing it is what allows it to be INLINED: a function with @c target
 * cannot be inlined into one that does not carry it, and that is the whole
 * reason there are two public entry points.  Without @c -mavx2 it stays as it
 * was: compiled apart and reached through a call, after asking the CPU.
 *
 * \~spanish
 * Con @c -mavx2 (o @c -march=native en una maquina que lo tenga) sobra el
 * atributo @c target -- el fichero entero ya se compila asi --, y quitarlo es
 * lo que permite METERLA EN LINEA: una funcion con @c target no se puede
 * inlinear en otra que no lo lleve, y esa es toda la razon de que existan dos
 * entradas publicas.  Sin @c -mavx2 se queda como estaba: compilada aparte y
 * alcanzada por una llamada, tras preguntar a la CPU.
 *
 * \~
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
 * @brief
 * \~english Stops the compiler from recognising the loop and replacing it WITH
 *          a call to @c memset / @c memcpy.
 * \~spanish Impide que el compilador reconozca el bucle y lo sustituya POR una
 *          llamada a @c memset / @c memcpy.
 * \~
 *
 * \~english
 * It sounds absurd and it is not: optimisers carry a pass that detects "this
 * loop fills a region" and swaps it for the library call, because normally that
 * is an improvement.  Here it is the opposite -- THIS IS the implementation --
 * and the result would be a recursion in disguise and, above all, the
 * dependency on libc that this whole folder exists in order not to have.
 *
 * CHECKED, not assumed: with @c vesta_memset_inline(p, 0, n), Clang leaves
 * @c memset unresolved in the object and GCC does not.  It is enough for the
 * pointer to be OPAQUE once per turn for the pass not to be able to recognise
 * the pattern; the asm is empty, so no instruction gets emitted.
 *
 * On GCC it is not put in because it is not needed: its equivalent pass
 * (@c -ftree-loop-distribute-patterns) only bit on the BYTE loops, which no
 * longer exist here -- see @c util/mem/mem_inline.h.
 *
 * \~spanish
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
 *
 * \~
 */
#if defined(VESTA_MEM_COMPILER_CLANG)
#define VESTA_MEM_KEEP_LOOP(p) __asm__ volatile("" : "+r"(p))
#else
#define VESTA_MEM_KEEP_LOOP(p) ((void)0)
#endif

/**
 * @def VESTA_MEM_NO_UNROLL
 * @brief
 * \~english Forbids the compiler from unrolling the loop that follows.
 * \~spanish Prohibe al compilador desenrollar el bucle que viene detras.
 * \~
 *
 * \~english
 * THE UNROLLING IS ALREADY DONE, and by hand: every turn moves 64, 128 or 256
 * bytes, and that number came out of measuring.  The compiler unrolling ON TOP
 * is walking over a decision that was already taken with data.
 *
 * And it is not theoretical.  DISASSEMBLED: when the size arrives as a constant
 * -- which is the normal thing from the typed interface -- Clang takes the turn
 * count to compile time and unfolds the whole loop.  A 4 KiB fill goes from 137
 * instructions to 407.  It trades a few branches for tripling the code, and at
 * that size that is not an improvement: it is instruction-cache pressure at
 * every place it gets called from.
 *
 * With this, what is emitted is what is written, and the only effect of knowing
 * the size and the alignment is the intended one: that checks become
 * unnecessary, not that code appears.
 *
 * \~spanish
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
 *
 * \~
 */
#if defined(VESTA_MEM_COMPILER_CLANG)
#define VESTA_MEM_NO_UNROLL _Pragma("clang loop unroll(disable)")
#elif defined(VESTA_MEM_COMPILER_GCC)
#define VESTA_MEM_NO_UNROLL _Pragma("GCC unroll 1")
#else
#define VESTA_MEM_NO_UNROLL
#endif

#endif // VESTA_UTIL_MEM_CONFIG_H

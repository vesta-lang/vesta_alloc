/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/mem/x86/x86_cpu.h
 * @brief
 * \~english What the CPU we are running on can do.
 * \~spanish Que sabe hacer la CPU en la que estamos corriendo.
 * \~
 *
 * \~english
 * This is what separates "the path is COMPILED" from "the path is USED".  The
 * AVX2 functions are always compiled on x86-64 -- with per-function @c target,
 * so the binary does not require AVX2 to start -- and who decides whether they
 * get called is this, at run time.
 *
 * Every new capability (AVX-512, fast @c rep @c movsb...) adds its question
 * here, and NOT in the copy's code: that way the dispatcher stays a list of
 * branches and the routines know nothing about detection.
 *
 * \~spanish
 * Esto es lo que separa "el camino esta COMPILADO" de "el camino se USA".  Las
 * funciones de AVX2 se compilan siempre en x86-64 -- con @c target por funcion,
 * asi que el binario no exige AVX2 para arrancar --, y quien decide si se
 * llaman es esto, en ejecucion.
 *
 * Cada capacidad nueva (AVX-512, @c rep @c movsb rapido...) anade aqui su
 * pregunta, y NO en el codigo de la copia: asi el despachador sigue siendo una
 * lista de ramas y las rutinas no saben nada de deteccion.
 *
 * \~
 */
#ifndef VESTA_UTIL_MEM_X86_CPU_H
#define VESTA_UTIL_MEM_X86_CPU_H

#include "util/mem/mem_config.h"

#if defined(VESTA_MEM_ARCH_X86_64)

/* Las instrucciones estan en su propia cabecera; aqui solo se interpretan sus
 * banderas.  Preguntar y decidir son dos cosas distintas. */
#include "util/mem/x86/x86_cpuid.h"

/* Una bandera por capacidad.  Anadir una es anadir su bit aqui y su linea en
 * `vesta_mem_x86_detect`, sin tocar nada mas: ni las rutinas ni el despacho. */
/// \~english it has been asked already  \~spanish ya se ha preguntado  \~
#define VESTA_MEM_X86_READY (1u << 0)
/// \~english 32-byte moves  \~spanish movimientos de 32 bytes  \~
#define VESTA_MEM_X86_AVX2 (1u << 1)
/// \~english FAST `rep movsb`/`rep stosb`
/// \~spanish `rep movsb`/`rep stosb` RAPIDOS  \~
#define VESTA_MEM_X86_ERMS (1u << 2)

/**
 * @brief
 * \~english What this CPU can do.  Zero while it has not been asked.
 * \~spanish Lo que sabe hacer esta CPU.  Cero mientras no se haya preguntado.
 * \~
 *
 * \~english
 * It is @c static, that is, one copy per translation unit.  It sounds wasteful
 * and it is not: four bytes and one more query per unit, in exchange for the
 * header needing no global symbol -- which is what lets it work in C, where
 * there are no @c inline variables.
 *
 * \~spanish
 * Es @c static, o sea una copia por unidad de traduccion.  Suena a desperdicio
 * y no lo es: son cuatro bytes y una consulta mas por unidad, a cambio de que
 * la cabecera no necesite ningun simbolo global -- que es lo que la deja valer
 * en C, donde no hay variables @c inline.
 *
 * \~
 */
static unsigned int vesta_mem_x86_features_state = 0;

/**
 * @brief
 * \~english Asks the CPU with @c CPUID about everything used here.  It is done
 *          once; the answer does not change.
 * \~spanish Pregunta a la CPU con @c CPUID por todo lo que aqui se usa.  Se
 *          hace una vez; el resultado no cambia.
 * \~
 *
 * \~english
 * WHY @c CPUID BY HAND AND NOT @c __builtin_cpu_supports.  Because the builtin
 * is not code, it is a CALL into the compiler's support library: it leaves
 * @c __cpu_model and @c __cpu_indicator_init unresolved, which libgcc provides.
 * That breaks the two things this library promises -- working without a system
 * library, and compiling with any compiler: with Clang targeting MSVC that
 * symbol does not exist and the LINK FAILS, checked.  @c CPUID depends on
 * nobody.
 *
 * AND THE CHECK THAT CANNOT BE SKIPPED is the operating system's, not the
 * CPU's.  The processor having AVX2 is not enough: the system has to be SAVING
 * the wide registers on a task switch, and that is asked with @c XGETBV.  If
 * AVX2 is used where the system does not save YMM, what gets corrupted is
 * another thread's registers -- a failure that does not look like a failure and
 * that shows up somewhere else entirely.  The builtin did this check inside; on
 * removing it, it has to be done here.
 *
 * The state is kept with the COMPILER's atomics (@c __atomic_store_n) and not
 * with `<atomic>`, because this header is also compiled by a C compiler -- and
 * along the way it does not drag a C++ header into a path that wants to be
 * freestanding.
 *
 * @par Threads
 * Safe.  Two threads arriving at once both ask and both write the SAME thing,
 * so the race cannot give an incorrect result and nothing else has to be
 * synchronised.
 *
 * \~spanish
 * POR QUE @c CPUID A MANO Y NO @c __builtin_cpu_supports.  Porque el builtin no
 * es codigo, es una LLAMADA a la biblioteca de soporte del compilador: deja
 * @c __cpu_model y @c __cpu_indicator_init sin resolver, que los pone libgcc.
 * Eso rompe las dos cosas que esta libreria promete -- funcionar sin biblioteca
 * de sistema, y compilar con cualquier compilador --: con Clang apuntando a
 * MSVC no existe ese simbolo y el ENLACE FALLA, comprobado.  @c CPUID no
 * depende de nadie.
 *
 * Y LA COMPROBACION QUE NO SE PUEDE SALTAR es la del sistema operativo, no la
 * de la CPU.  Que el procesador tenga AVX2 no basta: el sistema tiene que estar
 * GUARDANDO los registros anchos al cambiar de tarea, y eso se pregunta con
 * @c XGETBV.  Si se usa AVX2 donde el sistema no salva YMM, lo que se corrompe
 * son los registros de otro hilo -- un fallo que no se parece a un fallo y que
 * aparece en cualquier otro sitio --.  El builtin hacia esta comprobacion por
 * dentro; al quitarlo hay que hacerla aqui.
 *
 * El estado se guarda con los atomicos del COMPILADOR (@c __atomic_store_n) y
 * no con `<atomic>`, porque esta cabecera la compila tambien un compilador de
 * C -- y de paso no arrastra una cabecera de C++ a un camino que quiere ser
 * freestanding.
 *
 * @par Hilos
 * Segura.  Dos hilos que lleguen a la vez preguntan los dos y escriben lo
 * MISMO, asi que la carrera no puede dar un resultado incorrecto y no hace
 * falta sincronizar nada mas.
 *
 * \~
 * @return
 * \~english the @c VESTA_MEM_X86_* flags, always with @c READY set.
 * \~spanish las banderas @c VESTA_MEM_X86_*, siempre con @c READY puesto.
 * \~
 *
 * \~english
 * @code
 *   const unsigned int f = vesta_mem_x86_detect();   // forces the query
 * @endcode
 *
 * \~spanish
 * @code
 *   const unsigned int f = vesta_mem_x86_detect();   // fuerza la consulta
 * @endcode
 *
 * \~
 */
VESTA_MEM_INLINE unsigned int vesta_mem_x86_detect(void) VESTA_MEM_NOEXCEPT {
    unsigned int f = VESTA_MEM_X86_READY;

    /* Todo lo de aqui se anuncia en la hoja 7, asi que una CPU que no llegue
     * hasta ella no tiene nada que decir. */
    if (vesta_mem_x86_cpuid_max() >= 7) {
        const vesta_cpuid_regs f1 = vesta_mem_x86_cpuid(1, 0);
        const vesta_cpuid_regs f7 = vesta_mem_x86_cpuid(7, 0);

        /* OSXSAVE es el SISTEMA diciendo que ha habilitado XGETBV; AVX es la
         * CPU diciendo que tiene los registros anchos.  Hacen falta los dos, y
         * en ese orden: sin OSXSAVE ni siquiera se puede ejecutar XGETBV.
         * Bits 1 y 2 de XCR0: el sistema guarda el estado de XMM y el de YMM.
         * Los dos, no uno: con YMM sin guardar, AVX2 pisa registros ajenos. */
        const int osxsave = (f1.ecx & (1u << 27)) != 0;
        const int avx = (f1.ecx & (1u << 28)) != 0;
        if (osxsave && avx && (vesta_mem_x86_xgetbv(0) & 0x6u) == 0x6u) {
            if ((f7.ebx & (1u << 5)) != 0) f |= VESTA_MEM_X86_AVX2;
        }

        /* ERMS (bit 9) es la CPU diciendo que su `rep movsb` esta acelerado por
         * microcodigo.  NO se pregunta al sistema nada: `rep movsb` no usa
         * registros que haya que salvar, asi que no hay un XGETBV que valga. */
        if ((f7.ebx & (1u << 9)) != 0) f |= VESTA_MEM_X86_ERMS;
    }

    __atomic_store_n(&vesta_mem_x86_features_state, f, __ATOMIC_RELAXED);
    return f;
}

/**
 * @brief
 * \~english The same thing on the hot path: one relaxed load and one branch
 *          that is right every time but the first.
 * \~spanish Lo mismo por el camino caliente: una lectura relajada y una rama
 *          que acierta siempre menos la primera vez.
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
 * @return
 * \~english the @c VESTA_MEM_X86_* flags.
 * \~spanish las banderas @c VESTA_MEM_X86_*.
 * \~
 *
 * \~english
 * @code
 *   if (vesta_mem_x86_features() & VESTA_MEM_X86_AVX2) { ... }
 * @endcode
 *
 * \~spanish
 * @code
 *   if (vesta_mem_x86_features() & VESTA_MEM_X86_AVX2) { ... }
 * @endcode
 *
 * \~
 */
VESTA_MEM_ALWAYS_INLINE unsigned int
vesta_mem_x86_features(void) VESTA_MEM_NOEXCEPT {
    const unsigned int f =
        __atomic_load_n(&vesta_mem_x86_features_state, __ATOMIC_RELAXED);
    if ((f & VESTA_MEM_X86_READY) != 0) return f;
    return vesta_mem_x86_detect();
}

/**
 * @brief
 * \~english Can the 32-byte moves be used?
 * \~spanish Se pueden usar los movimientos de 32 bytes?
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
 * @return
 * \~english other than 0 when the CPU has AVX2 and the system backs it.
 * \~spanish distinto de 0 si la CPU tiene AVX2 y el sistema lo respalda.
 * \~
 *
 * \~english
 * @code
 *   if (vesta_mem_x86_has_avx2()) vesta_mem_avx2_copy(d, s, n);
 * @endcode
 *
 * \~spanish
 * @code
 *   if (vesta_mem_x86_has_avx2()) vesta_mem_avx2_copy(d, s, n);
 * @endcode
 *
 * \~
 */
VESTA_MEM_ALWAYS_INLINE int vesta_mem_x86_has_avx2(void) VESTA_MEM_NOEXCEPT {
#if defined(VESTA_MEM_TARGET_AVX2)
    /* La micro-ISA vino fijada al compilar: el binario no arranca en una
     * maquina sin AVX2, asi que la respuesta es constante y con ella se pliegan
     * tambien la rama y todo el camino que no se toma. */
    return 1;
#else
    return (vesta_mem_x86_features() & VESTA_MEM_X86_AVX2) != 0;
#endif
}

/**
 * @brief
 * \~english Is this CPU's @c rep @c movsb fast?
 * \~spanish Es rapido el @c rep @c movsb de esta CPU?
 * \~
 *
 * \~english
 * The question matters because @c rep @c movsb has existed since the 8086 and
 * on old machines it is SLOW -- it runs byte by byte.  From ERMS on, the
 * microcode turns it into the fastest loop the CPU knows how to do, one that
 * also knows its own widths and its own cache hierarchy.
 *
 * @par Threads
 * Safe.
 *
 * \~spanish
 * La pregunta importa porque @c rep @c movsb existe desde el 8086 y en las
 * maquinas viejas es LENTO -- se ejecuta byte a byte --.  A partir de ERMS el
 * microcodigo lo convierte en el bucle mas rapido que la CPU sabe hacer, que
 * ademas conoce sus propios anchos y su propia jerarquia de cache.
 *
 * @par Hilos
 * Segura.
 *
 * \~
 * @return
 * \~english other than 0 when @c rep @c movsb is accelerated.
 * \~spanish distinto de 0 si @c rep @c movsb esta acelerado.
 * \~
 *
 * \~english
 * @code
 *   if (vesta_mem_x86_has_erms()) vesta_mem_erms_copy(d, s, n);
 * @endcode
 *
 * \~spanish
 * @code
 *   if (vesta_mem_x86_has_erms()) vesta_mem_erms_copy(d, s, n);
 * @endcode
 *
 * \~
 */
VESTA_MEM_ALWAYS_INLINE int vesta_mem_x86_has_erms(void) VESTA_MEM_NOEXCEPT {
#if defined(VESTA_MEM_TARGET_ERMS)
    /* Aqui no hay un `__ERMS__` que mirar -- no es una extension de conjunto de
     * instrucciones, es una propiedad del microcodigo --, asi que esto solo se
     * activa a mano, para quien sepa en que maquina va a ejecutar. */
    return 1;
#else
    return (vesta_mem_x86_features() & VESTA_MEM_X86_ERMS) != 0;
#endif
}

/**
 * @def VESTA_MEM_X86_LLC_UNKNOWN
 * @brief
 * \~english What @c vesta_mem_x86_llc_bytes answers when the CPU does not say.
 * \~spanish Lo que contesta @c vesta_mem_x86_llc_bytes cuando la CPU no lo
 *           dice.
 * \~
 *
 * \~english
 * It is 4 GiB minus one and not zero ON PURPOSE.  Whoever asks is comparing a
 * size against it to decide whether to bypass the caches, and a huge answer
 * makes every real block fall on the SAFE side -- the path that is used today
 * -- with no special case to write and none to forget.  Zero would do the
 * opposite and turn "the CPU did not say" into "bypass everything".
 *
 * \~spanish
 * Es 4 GiB menos uno y no cero A PROPOSITO.  Quien pregunta compara un tamano
 * contra esto para decidir si se salta las caches, y una respuesta enorme deja
 * todo bloque real del lado SEGURO -- el camino de hoy --, sin ningun caso
 * especial que escribir ni que olvidar.  Cero haria lo contrario y convertiria
 * "la CPU no lo dijo" en "saltarselo todo".
 *
 * \~
 */
#define VESTA_MEM_X86_LLC_UNKNOWN 0xFFFFFFFFu

/// \~english The last level cache, in bytes.  Zero while it has not been asked.
/// \~spanish La cache de ultimo nivel, en bytes.  Cero mientras no se pregunte.
/// \~
static unsigned int vesta_mem_x86_llc_state = 0;

/**
 * @brief
 * \~english Asks @c CPUID how big the last level of cache is.
 * \~spanish Pregunta con @c CPUID cuanto mide el ultimo nivel de cache.
 * \~
 *
 * \~english
 * WHAT IT IS FOR, because a cache size is a strange thing for a memory
 * primitive to want: it is the line above which a fill should stop going
 * through the caches.  Writing through them reads every line before overwriting
 * it and evicts whatever was there; a streaming store does neither.  Below the
 * cache that trade is bad -- the block was going to be useful where it landed
 * -- and above it there is nothing to keep, so the read is pure waste.
 * Measured on Raptor Lake, filling and then reading the block back: streaming
 * stops losing on ANY fraction read between 24 and 28 MiB, with a 30 MB cache.
 * So the threshold is the cache, and it is asked for rather than written down.
 *
 * The walk is @c CPUID leaf 4, which describes one cache per subleaf until it
 * reports type 0.  AMD says the same thing in leaf @c 0x8000001D with the same
 * layout, so the same loop reads both; the older AMD leaf @c 0x80000006 is not
 * consulted, and a CPU that only has that one comes out as unknown, which lands
 * on the safe path.
 *
 * @par Threads
 * Safe.  Two threads racing compute the SAME value from the same instruction
 * and store it; there is no state that a torn read could break.
 *
 * \~spanish
 * PARA QUE SIRVE, porque el tamano de una cache es algo raro que querer en una
 * primitiva de memoria: es la raya por encima de la cual un relleno debe dejar
 * de pasar por las caches.  Escribir por ellas lee cada linea antes de
 * sobreescribirla y desaloja lo que hubiera; un almacen no temporal no hace
 * ninguna de las dos.  Por debajo de la cache ese cambio es malo -- el bloque
 * iba a servir donde cayo -- y por encima no hay nada que conservar, asi que la
 * lectura se tira.  Medido en Raptor Lake, rellenando y leyendo despues el
 * bloque: el no temporal deja de perder en CUALQUIER fraccion leida entre 24 y
 * 28 MiB, con una cache de 30 MB.  Asi que el umbral es la cache, y se
 * pregunta en vez de escribirse.
 *
 * El recorrido es la hoja 4 de @c CPUID, que describe una cache por subhoja
 * hasta contestar tipo 0.  AMD dice lo mismo en la hoja @c 0x8000001D con el
 * mismo formato, asi que el mismo bucle lee las dos; la hoja vieja de AMD
 * @c 0x80000006 no se consulta, y una CPU que solo tenga esa sale como
 * desconocida, que cae en el camino seguro.
 *
 * @par Hilos
 * Segura.  Dos hilos a la vez calculan el MISMO valor de la misma instruccion y
 * lo guardan; no hay estado que una lectura partida pueda romper.
 *
 * \~
 * @return
 * \~english the size in bytes, or @c VESTA_MEM_X86_LLC_UNKNOWN.
 * \~spanish el tamano en bytes, o @c VESTA_MEM_X86_LLC_UNKNOWN.
 * \~
 */
VESTA_MEM_INLINE unsigned int
vesta_mem_x86_detect_llc(void) VESTA_MEM_NOEXCEPT {
    unsigned int best = 0;      // bytes of the deepest cache seen
    unsigned int best_level = 0; // and which level that was

    /* Intel's leaf and AMD's say the same thing in the same registers, so the
     * loop is written once and pointed at whichever exists. */
    uint32_t leaf = 0;
    if (vesta_mem_x86_cpuid_max() >= 4) {
        leaf = 4;
    } else if (vesta_mem_x86_cpuid(0x80000000u, 0).eax >= 0x8000001Du) {
        leaf = 0x8000001Du;
    }

    if (leaf != 0) {
        /* A bound and not `while (1)`: a CPU that never reports type 0 -- a
         * virtual machine making it up, which is where this runs today --
         * would spin here forever.  Sixteen is far past any real topology. */
        for (uint32_t i = 0; i < 16; ++i) {
            const vesta_cpuid_regs c = vesta_mem_x86_cpuid(leaf, i);
            const unsigned int type = c.eax & 0x1Fu;
            if (type == 0) break;          // no more caches described
            if (type == 2) continue;       // instructions: not what fills go to
            const unsigned int level = (c.eax >> 5) & 0x7u;
            if (level < best_level) continue;

            /* size = ways * partitions * line * sets, every field stored one
             * less than it is. */
            const unsigned int ways = ((c.ebx >> 22) & 0x3FFu) + 1u;
            const unsigned int parts = ((c.ebx >> 12) & 0x3FFu) + 1u;
            const unsigned int line = (c.ebx & 0xFFFu) + 1u;
            const unsigned int sets = c.ecx + 1u;
            const unsigned int bytes = ways * parts * line * sets;
            if (bytes != 0) {
                best = bytes;
                best_level = level;
            }
        }
    }

    const unsigned int answer = best != 0 ? best : VESTA_MEM_X86_LLC_UNKNOWN;
    __atomic_store_n(&vesta_mem_x86_llc_state, answer, __ATOMIC_RELAXED);
    return answer;
}

/**
 * @brief
 * \~english How big the last level of cache is, in bytes.
 * \~spanish Cuanto mide el ultimo nivel de cache, en bytes.
 * \~
 *
 * \~english
 * The same shape as @c vesta_mem_x86_features: one relaxed load and a branch
 * that is right every time but the first.
 *
 * @par Threads
 * Safe.
 *
 * \~spanish
 * La misma forma que @c vesta_mem_x86_features: una lectura relajada y una rama
 * que acierta siempre menos la primera vez.
 *
 * @par Hilos
 * Segura.
 *
 * \~
 * @return
 * \~english the size in bytes, or @c VESTA_MEM_X86_LLC_UNKNOWN when the CPU
 *           does not describe it.
 * \~spanish el tamano en bytes, o @c VESTA_MEM_X86_LLC_UNKNOWN cuando la CPU no
 *           lo describe.
 * \~
 *
 * \~english
 * @code
 *   if (n >= vesta_mem_x86_llc_bytes()) { ... }   // past the caches
 * @endcode
 *
 * \~spanish
 * @code
 *   if (n >= vesta_mem_x86_llc_bytes()) { ... }   // pasada la cache
 * @endcode
 *
 * \~
 */
VESTA_MEM_ALWAYS_INLINE unsigned int
vesta_mem_x86_llc_bytes(void) VESTA_MEM_NOEXCEPT {
    const unsigned int v =
        __atomic_load_n(&vesta_mem_x86_llc_state, __ATOMIC_RELAXED);
    if (v != 0) return v;
    return vesta_mem_x86_detect_llc();
}

#endif // VESTA_MEM_ARCH_X86_64

#endif // VESTA_UTIL_MEM_X86_CPU_H

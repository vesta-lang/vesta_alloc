/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file src/interpose/interpose_common.h
 * @brief
 * \~english What the two ways of becoming `malloc` both need.
 * \~spanish Lo que necesitan las dos vias de ser `malloc`.
 * \~
 *
 * \~english
 * THERE ARE TWO WAYS, and which one is in force is decided when the library is
 * built.  They answer different questions, so neither replaces the other:
 *
 *   - `malloc_interpose.cpp` -- renaming at LINK time (`-Wl,--wrap=`).  Reaches
 *     every call in every object of the link and nothing else.  Portable, and
 *     the only one that works on Windows.
 *   - `malloc_define.cpp` -- DEFINING the symbol, the way jemalloc and tcmalloc
 *     do.  On ELF it reaches, on top of that, what the C library allocates
 *     INSIDE itself and hands back (`strdup`, `getline`, `asprintf`), which is
 *     the hole the other one cannot close.  ELF only.
 *
 * They cannot both be in force for the same symbol, and the reason is worth
 * writing down because it is not obvious and it is easy to try: with
 * `--wrap=malloc` AND a definition of `malloc`, the linker resolves
 * `__real_malloc` against the only definition of `malloc` there is -- OURS --
 * and the first allocation recurses until the stack runs out.  Measured, not
 * reasoned: three levels deep and gone.  So the build turns off the renaming
 * for exactly the symbols the other file defines.
 *
 * What lives here is the part that would otherwise be copied into both, and the
 * copy is the dangerous kind: `note_site` decides whether an allocation shows
 * up in the report at all, so two versions of it drifting apart would produce a
 * report that is wrong without looking incomplete.
 *
 * \~spanish
 * HAY DOS VIAS, y cual esta en vigor se decide al construir la libreria.
 * Contestan preguntas distintas, asi que ninguna sustituye a la otra:
 *
 *   - `malloc_interpose.cpp` -- renombrar al ENLAZAR (`-Wl,--wrap=`).  Alcanza
 *     toda llamada de todo objeto del enlace y nada mas.  Portable, y la unica
 *     que funciona en Windows.
 *   - `malloc_define.cpp` -- DEFINIR el simbolo, como hacen jemalloc y
 *     tcmalloc.  En ELF alcanza, ademas, lo que la libreria de C reserva POR
 *     DENTRO y devuelve (`strdup`, `getline`, `asprintf`), que es el agujero
 *     que la otra no puede cerrar.  Solo ELF.
 *
 * No pueden estar las dos en vigor para el mismo simbolo, y la razon merece
 * quedar escrita porque no es obvia y es facil intentarlo: con `--wrap=malloc`
 * Y una definicion de `malloc`, el enlazador resuelve `__real_malloc` con la
 * unica definicion de `malloc` que hay -- la NUESTRA -- y la primera reserva se
 * llama a si misma hasta agotar la pila.  Medido, no razonado: tres niveles y
 * fuera.  Por eso el build apaga el renombrado exactamente para los simbolos
 * que define el otro fichero.
 *
 * Lo que vive aqui es la parte que si no habria que copiar en los dos, y la
 * copia es de las peligrosas: `note_site` decide si una reserva llega siquiera
 * al informe, asi que dos versiones separandose darian un informe equivocado
 * sin parecer incompleto.
 * \~
 */
#ifndef VESTA_ALLOC_INTERPOSE_COMMON_H
#define VESTA_ALLOC_INTERPOSE_COMMON_H

#include "util/report/alloc_sites.h"
#include "util/interpose/call_site.h"
#include "util/alloc/host_allocator.h"
#include "util/alloc/host_allocator_layout.h"
/* \~english Our own copy, not the system's: in this library memory moves with
 * our primitives, and besides, `memcpy` is one of the functions an interposed
 * allocator does not want to be calling from inside itself.
 *
 * \~spanish Nuestra copia, no la del sistema: en esta libreria la memoria se
 * mueve con nuestras primitivas, y ademas `memcpy` es una de las funciones que
 * un asignador interpuesto no quiere estar llamando desde dentro.  \~ */
#include "util/mem/vesta_memcpy.h"
#include "util/mem/vesta_memset.h"

#include <cstddef>

namespace vesta_interpose {

/**
 * @brief
 * \~english Did this block come out of the allocator.
 * \~spanish Si este bloque salio del asignador.
 * \~
 *
 * \~english
 * Two comparisons, no table and no lock, which is the whole reason freeing is
 * cheap here -- and they are still the only thing the common case runs.
 *
 * WHAT THE TABLE IS FOR.  Everything over `kMaxSpanBytes` is served by the
 * system on a reservation of its own, so it falls outside BOTH regions and the
 * two comparisons say no.  Before, that answer was final and the block went to
 * `no_foreign_free`, which stops the process: `free()` of a 16 MiB `malloc`
 * would have killed any program running with the interposition on.  So when
 * they say no -- and only then, on the branch that was already about to end
 * the program -- the table is asked, and it places the block without reading
 * it, which is what a pointer that might be foreign requires.
 *
 * \~spanish
 * Dos comparaciones, sin tabla y sin cerrojo, que es toda la razon de que
 * liberar salga barato aqui -- y siguen siendo lo unico que ejecuta el caso
 * comun.
 *
 * PARA QUE ESTA LA TABLA.  Todo lo que pasa de `kMaxSpanBytes` lo sirve el
 * sistema en una reserva propia, asi que cae fuera de las DOS regiones y las
 * dos comparaciones dicen que no.  Antes esa respuesta era definitiva y el
 * bloque se iba a `no_foreign_free`, que para el proceso: un `free()` de un
 * `malloc` de 16 MiB habria matado cualquier programa que corriera con la
 * interposicion puesta.  Asi que cuando dicen que no -- y solo entonces, en la
 * rama que ya iba a terminar el programa -- se pregunta a la tabla, que situa
 * el bloque SIN leerlo, que es lo que exige un puntero que podria ser ajeno.
 * \~
 */
[[gnu::always_inline]] inline bool ours(const void *p) noexcept {
    if (__builtin_expect(util::in_region(p) || util::in_big_region(p), 1))
        return true;
    return util::detail::direct_bytes(p) != 0;
}

/**
 * @brief
 * \~english A power of two, and not zero.
 * \~spanish Una potencia de dos, y no cero.
 * \~
 *
 * \~english
 * Every aligned entry checks this first, and not as a formality: the rounding
 * is a mask, so an alignment that is not a power of two would not round -- it
 * would hand back an address that is not aligned and looks like it is.
 *
 * \~spanish
 * Toda entrada alineada comprueba esto primero, y no por formalidad: el
 * redondeo es una mascara, asi que una alineacion que no sea potencia de dos no
 * redondearia -- devolveria una direccion que no esta alineada y lo parece.
 * \~
 */
[[gnu::always_inline]] inline bool pow2(size_t a) noexcept {
    return a != 0 && (a & (a - 1)) == 0;
}

/**
 * @brief
 * \~english Notes where this allocation came from, when sites were asked for.
 * \~spanish Apunta de donde vino esta reserva, cuando se han pedido los sitios.
 * \~
 *
 * \~english
 * @p ret is the return address of the interposed entry -- the instruction after
 * the `call malloc` in the CALLER.  It is the same thing the `operator new`
 * patch reads off `[rsp]`, so a `malloc` and a `new` from the same function
 * land on one site instead of two.
 *
 * The condition is the same one the C++ path uses, deliberately: measuring
 * (`VESTA_HOST_ALLOC_STATS`) and recording sites (`VESTA_HOST_ALLOC_SITES`) are
 * separate requests, and the patch over `operator new` only goes in for the
 * second.  Recording here on the first alone would fill the table with `malloc`
 * and with nothing from `new` -- a list that looks complete and leaves out half
 * the program.
 *
 * \~spanish
 * @p ret es la direccion de retorno de la entrada interpuesta -- la instruccion
 * de despues del `call malloc` en el LLAMANTE --.  Es lo mismo que el parche de
 * `operator new` lee de `[rsp]`, asi que un `malloc` y un `new` de la misma
 * funcion caen en un solo sitio en vez de dos.
 *
 * La condicion es la misma que usa el camino de C++, y a proposito: medir
 * (`VESTA_HOST_ALLOC_STATS`) y apuntar sitios (`VESTA_HOST_ALLOC_SITES`) son
 * peticiones distintas, y el parche sobre `operator new` solo entra para la
 * segunda.  Apuntar aqui con solo la primera llenaria la tabla de `malloc` y de
 * nada de `new` -- una lista que parece completa y se deja fuera medio
 * programa.
 * \~
 *
 * @warning
 * \~english The address is only as good as the call that produced it.  A caller
 * that ends in `return malloc(n);` is compiled as a TAIL CALL by GCC and Clang
 * alike, and then there is no return address of its own to read: what arrives
 * here belongs to ITS caller.  Nothing in this file can tell the difference.
 *
 * \~spanish La direccion vale lo que valga la llamada que la produjo.  Un
 * llamante que acaba en `return malloc(n);` lo compilan como LLAMADA DE COLA
 * tanto GCC como Clang, y entonces no hay direccion de retorno propia que leer:
 * lo que llega aqui es la de SU llamante.  Nada en este fichero puede
 * distinguirlo.
 * \~
 */
inline void note_site(const void *ret, size_t n) noexcept {
    if (!util::detail::g_measure || !util::call_site_patch_installed()) return;
    const util::detail::ThreadCache *c = util::detail::current_cache();
    /* \~english `have_cache` and not a null test: a thread past its exit notice
     * carries a marker in the slot, and reading `c->tag` off it faults.  The C
     * runtime's own teardown allocates -- that is the whole reason this file
     * exists -- so this is not a corner case, it is every thread.  See
     * `util::detail::kDyingCache`.
     *
     * \~spanish `have_cache` y no una comprobacion de nulo: un hilo pasado su
     * aviso de fin lleva una marca en la ranura, y leerle `c->tag` falla.  El
     * cierre del propio runtime de C reserva -- que es toda la razon de que
     * este fichero exista --, asi que no es un caso raro: es todos los hilos.
     * Ver `util::detail::kDyingCache`.  \~ */
    util::record_alloc_site(ret, n,
                            util::detail::have_cache(c) ? c->tag : 0);
}

/* --------------------------------------------------------------------------
 *  \~english The bootstrap arena, seen from the OTHER side.
 *
 *  Only `malloc_define.cpp` serves out of it, but every path that FREES or
 *  REALLOCATES has to recognise one of its blocks -- handing one to the C
 *  library would be handing it a pointer it never made.  That is why these two
 *  are declared here and not kept private: the arena is not a private detail of
 *  whoever fills it, it is a third kind of block that exists process-wide.
 *
 *  With the option off there is no arena, and these collapse to a constant
 *  `false` and a zero that the optimiser removes outright -- so the ordinary
 *  build does not carry a branch for a thing that cannot happen.
 *
 *  \~spanish La arena de arranque, vista desde el OTRO lado.
 *
 *  Solo `malloc_define.cpp` sirve de ella, pero todo camino que LIBERE o
 *  RECOLOQUE tiene que reconocer uno de sus bloques -- darle uno a la libreria
 *  de C seria darle un puntero que ella no hizo --.  Por eso estas dos se
 *  declaran aqui y no se quedan privadas: la arena no es un detalle privado de
 *  quien la llena, es una TERCERA clase de bloque que existe en todo el
 *  proceso.
 *
 *  Con la opcion apagada no hay arena, y estas se quedan en un `false`
 *  constante y un cero que el optimizador borra del todo -- asi que el build de
 *  siempre no carga con una rama para algo que no puede pasar.
 * \~ ------------------------------------------------------------------- */
#if defined(VESTA_ALLOC_DEFINE_MALLOC)

/// Whether @p p was served by the bootstrap arena.
bool from_bootstrap(const void *p) noexcept;
/// How many useful bytes that block has.  Only valid for a bootstrap block.
size_t bootstrap_size(const void *p) noexcept;

#else

[[gnu::always_inline]] inline bool from_bootstrap(const void *) noexcept {
    return false;
}
[[gnu::always_inline]] inline size_t bootstrap_size(const void *) noexcept {
    return 0;
}

#endif

} // namespace vesta_interpose

#endif // VESTA_ALLOC_INTERPOSE_COMMON_H

/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file util/interpose/msvcrt_hook.h
 * @brief
 * \~english Making what the C runtime allocates INSIDE itself ours, on Windows.
 * \~spanish Hacer nuestro lo que el runtime de C reserva DENTRO de si mismo, en
 *          Windows.
 * \~
 *
 * \~spanish
 * POR QUE HAY UN TERCER MECANISMO.  Ya existen dos, y ninguno llega hasta aqui:
 * el renombrado al enlazar alcanza las llamadas del enlace, y los punteros
 * `__imp__` alcanzan las que pasan por la tabla de importaciones.  Lo que hace
 * `_strdup` cuando llama a `malloc` no es ninguna de las dos -- es una funcion
 * de msvcrt llamando a otra funcion de msvcrt, una llamada relativa dentro de
 * la DLL --.  No hay referencia que renombrar ni entrada de importacion que
 * redirigir.
 *
 * En ELF ese hueco se cierra definiendo el simbolo, porque las llamadas propias
 * de una libreria compartida salen por la PLT.  Una DLL no tiene PLT, asi que
 * el unico sitio que queda es el codigo, y lo que va ahi es una instruccion: un
 * salto en la entrada de `malloc` apuntando al nuestro.  Lo mismo que esta
 * libreria ya escribe sobre `operator new` para apuntar los sitios de llamada.
 *
 * SOLO se toca msvcrt.  Ni kernel32, ni ntdll, ni ningun otro componente: el
 * runtime de C es el que reserva por cuenta del programa.
 *
 * SE INSTALA SOLO, desde un constructor, tan pronto como esta libreria ejecuta
 * algo -- cuanto antes entre, menos bloques ha hecho ya el runtime que luego
 * hay que devolverle por su monton en vez de que sean simplemente nuestros --.
 * Estos puntos de entrada son para preguntar si funciono, y para ponerlo a mano
 * en una compilacion que no lo quisiera automatico.
 *
 * \~english
 * WHY THERE IS A THIRD MECHANISM.  Two already exist, and neither reaches this:
 * link-time renaming reaches the calls in the link, and the `__imp__` pointers
 * reach the ones that go through the import table.  What `_strdup` does when it
 * calls `malloc` is neither -- it is one function of msvcrt calling another
 * function of msvcrt, a relative call inside the DLL.  There is no reference to
 * rename and no import entry to redirect.
 *
 * On ELF that hole closes by defining the symbol, because a shared library's
 * own calls go out through the PLT.  A DLL has no PLT, so the only place left
 * is the code, and what goes there is one instruction: a jump at the entry of
 * `malloc` pointing at ours.  The same thing this library already writes over
 * `operator new` to record call sites.
 *
 * ONLY msvcrt is touched.  Not kernel32, not ntdll, not any other component:
 * the C runtime is the one that allocates on the program's behalf.
 *
 * INSTALLED BY ITSELF, from a constructor, as early as this library runs
 * anything -- the earlier it goes in, the fewer blocks the runtime has already
 * made that then have to be handed back through its heap instead of simply
 * being ours.  These entry points are for asking whether it worked, and for
 * putting it in by hand in a build that did not want it automatic.
 *
 * \~
 */
#ifndef VESTA_UTIL_MSVCRT_HOOK_H
#define VESTA_UTIL_MSVCRT_HOOK_H

namespace util {

/**
 * @brief
 * \~english Redirects the C runtime's memory family, and the environment with
 *           it.
 * \~spanish Redirige la familia de memoria del runtime de C, y el entorno con
 *           ella.
 * \~
 *
 * \~english
 * Idempotent: asking twice does nothing the second time.
 *
 * WHICH FUNCTIONS.  The four that allocate -- `malloc`, `calloc`, `realloc`,
 * `free` -- and the two that only ASK about a block, `_msize` and `_expand`.
 * Those last two allocate nothing, which is exactly why they were missed: they
 * still hand the caller's pointer to the NT heap, and doing that with one of
 * our blocks stops the process for heap corruption.  Serving a block and
 * leaving the questions about it to somebody else is being `malloc` by halves.
 * Nothing else in msvcrt reaches the heap except through those six; the sweep
 * that establishes it is written down in the implementation.
 *
 * \~spanish
 * Idempotente: pedirlo dos veces no hace nada la segunda.
 *
 * QUE FUNCIONES.  Las cuatro que reservan -- `malloc`, `calloc`, `realloc`,
 * `free` -- y las dos que solo PREGUNTAN por un bloque, `_msize` y `_expand`.
 * Esas dos ultimas no reservan nada, que es justo por lo que se pasaron por
 * alto: aun asi le dan al monton NT el puntero de quien llama, y hacerlo con
 * uno de nuestros bloques para el proceso por corrupcion del monton.  Servir un
 * bloque y dejarle a otro las preguntas sobre el es ser `malloc` a medias.
 * Nada mas de msvcrt llega al monton si no es por esas seis; el barrido que lo
 * establece esta escrito en la implementacion.
 *
 * \~
 * @return
 * \~english false if msvcrt is not loaded, or if any of the jumps would not go
 *           in.  It is SAID and not swallowed -- a hook that went in halfway is
 *           worse than one that did not go in at all, because half the pairs
 *           would be split between two allocators.
 * \~spanish false si msvcrt no esta cargada, o si alguno de los saltos no pudo
 *           entrar.  Se DICE y no se traga: un gancho que entro a medias es
 *           peor que uno que no entro, porque la mitad de los pares quedarian
 *           repartidos entre dos asignadores.
 * \~
 *
 * @par Threads
 * \~english Meant to run while there is a single thread, which is what a
 * constructor guarantees.  Writing over the entry of a function that another
 * thread might be inside is not made safe by a lock.
 * \~spanish Pensada para correr con un solo hilo, que es lo que garantiza un
 * constructor.  Escribir sobre la entrada de una funcion dentro de la que
 * puede haber otro hilo no se arregla con un cerrojo.
 * \~
 */
bool install_msvcrt_hook() noexcept;

/**
 * @brief
 * \~english Whether the hook is in force.
 * \~spanish Si el gancho esta puesto.
 * \~
 *
 * \~english
 * For whoever wants to know instead of assume: with it off, the runtime's
 * internal allocations are not in the report, and a report that is missing
 * them looks complete.
 *
 * \~spanish
 * Para quien quiera saberlo en vez de suponerlo: con el apagado, lo que el
 * runtime reserva por dentro no sale en el informe, y un informe al que le
 * falta eso parece completo.
 *
 * \~
 * @return
 * \~english true when every jump went in.
 * \~spanish true cuando entraron todos los saltos.
 * \~
 */
bool msvcrt_hook_installed() noexcept;

/**
 * @brief
 * \~english Blocks the runtime made BEFORE the hook that nobody could take
 *           back.
 * \~spanish Bloques que el runtime hizo ANTES del gancho y que nadie pudo
 *           recuperar.
 * \~
 *
 * \~english
 * The runtime starts before we do, so some of its memory reaches `free` after
 * the patch is in.  Those go back to its own heap directly; this counts the
 * ones where even that was not possible.  It is bounded by what was allocated
 * before the constructor ran, so it does not grow with the program -- and it
 * is counted rather than ignored because a leak nobody measures cannot be told
 * apart from no leak at all.
 *
 * \~spanish
 * El runtime arranca antes que nosotros, asi que parte de su memoria llega a
 * `free` con el parche ya puesto.  Esos vuelven a su propio monton
 * directamente; esto cuenta aquellos en los que ni eso fue posible.  Esta
 * acotado por lo que se reservo antes de que corriera el constructor, asi que
 * no crece con el programa -- y se cuenta en vez de ignorarse porque una fuga
 * que nadie mide no se distingue de no tener ninguna.
 *
 * \~
 * @return
 * \~english how many of them there are, which does not grow with the program.
 * \~spanish cuantos son, que no crece con el programa.
 * \~
 */
unsigned long long msvcrt_stranded_blocks() noexcept;

} // namespace util

#endif // VESTA_UTIL_MSVCRT_HOOK_H

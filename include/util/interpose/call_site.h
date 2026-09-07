/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file util/interpose/call_site.h
 * @brief
 * \~english WHERE each allocation comes from, at no cost to the normal path.
 * \~spanish DE DONDE sale cada reserva, sin coste para el camino normal.
 * \~
 *
 * \~spanish
 * EL PROBLEMA.  Para apuntar quien reserva hace falta la direccion de retorno
 * de `operator new`.  Antes salia de `__builtin_return_address(0)`, que SI es
 * fiable para el cero -- pero es un valor que te entrega el compilador, y en
 * cuanto se quiere subir un marco mas el propio GCC dice de
 * `__builtin_return_address(N>0)` que "puede tener efectos impredecibles,
 * incluido tumbar el programa que llama".
 *
 * COMO DEJAMOS DE DEPENDER DE NADIE.  La convencion de llamada dice que AL
 * ENTRAR en una funcion la direccion de retorno esta en `[rsp]`.  Un `mov`.  Ni
 * se busca por la pila, ni hay heuristicas, ni hay nada que el optimizador
 * pueda cambiar.  Lo unico que hace falta es que el prologo sea NUESTRO, que es
 * lo que da un trampolin en ensamblador.
 *
 * Y POR QUE UN PARCHE.  Porque un trampolin permanente lo pagarian TODAS las
 * reservas, incluidas las de quien no esta midiendo: un `mov` y un `jmp` por
 * `new`.  En este asignador eso no es despreciable -- mover la medicion al
 * camino frio ya valio un 3,3% medido --.  Asi que `operator new` se queda
 * exactamente como estaba, y cuando se pide medir se escribe un salto sobre su
 * entrada.  Con eso apagado no hay nada que pagar porque no hay nada.
 *
 * LO QUE NO RESUELVE.  La direccion es la del llamante INMEDIATO.  Para un
 * `std::string` ese llamante es la biblioteca estandar, no el codigo que queria
 * la cadena.  Esto hace que el valor sea NUESTRO y exacto; llegar mas arriba es
 * otra conversacion.
 *
 * \~english
 * THE PROBLEM.  To record who allocates you need the return address of
 * `operator new`.  It used to come from `__builtin_return_address(0)`, which IS
 * reliable for zero -- but it is a value the compiler hands you, and the moment
 * you want to walk one frame further up GCC itself says of
 * `__builtin_return_address(N>0)` that it "may have unpredictable effects,
 * including crashing the calling program".
 *
 * HOW WE STOP DEPENDING ON ANYONE.  The calling convention says that ON ENTRY
 * to a function the return address sits at `[rsp]`.  One `mov`.  No stack
 * searching, no heuristics, and nothing the optimiser can change.  All it takes
 * is for the prologue to be OURS, which is what an assembly thunk gives us.
 *
 * AND WHY A PATCH.  Because a permanent thunk would be paid by EVERY
 * allocation, including those of people who are not measuring: one `mov` and
 * one `jmp` per `new`.  In this allocator that is not negligible -- moving the
 * measurement into the cold path was already worth a measured 3.3% --.  So
 * `operator new` stays exactly as it was, and when measurement is requested a
 * jump is written over its entry.  With it off there is nothing to pay because
 * there is nothing there.
 *
 * WHAT IT DOES NOT SOLVE.  The address is that of the IMMEDIATE caller.  For a
 * `std::string` that caller is the standard library, not the code that wanted
 * the string.  This makes the value OURS and exact; reaching further up is a
 * separate conversation.
 *
 * \~
 */
#ifndef VESTA_UTIL_CALL_SITE_H
#define VESTA_UTIL_CALL_SITE_H

namespace util {

/**
 * @brief
 * \~english Makes the `operator new` family record where they were called
 *          from.
 * \~spanish Hace que la familia de `operator new` apunte desde donde se la
 *          llamo.
 * \~
 *
 * \~english
 * Writes a jump to the thunk over each one's entry point.  Until this is
 * called, `operator new` is the usual one and costs NOT ONE extra instruction:
 * that is the whole reason this is a patch and not a permanent thunk.
 *
 * Covers all EIGHT: `new`, `new[]`, their two `nothrow` forms, and the four
 * over-aligned ones.  Leaving the over-aligned ones out was the kind of hole
 * you cannot see -- anything aligned to a cache line did not show up in the
 * report as "undeclared", it did not show up at all -- and a list of who
 * allocates that is missing allocations is not incomplete, it is wrong.
 *
 * WHEN TO CALL IT: once, when measurement is switched on, BEFORE the first
 * allocation and with a single thread running.  It is not meant to be switched
 * on hot -- it writes over code other threads could be executing -- which is
 * why a second call does nothing.
 *
 * \~spanish
 * Escribe un salto al trampolin sobre el punto de entrada de cada uno.  Hasta
 * que se llama a esto, `operator new` es el de siempre y no cuesta NI UNA
 * instruccion de mas: esa es toda la razon de que sea un parche y no un
 * trampolin permanente.
 *
 * Cubre los OCHO: `new`, `new[]`, sus dos formas `nothrow` y los cuatro
 * sobrealineados.  Dejar fuera los sobrealineados era la clase de hueco que no
 * se ve -- lo alineado a una linea de cache no salia en el informe como "sin
 * declarar", es que no salia -- y una lista de quien reserva a la que le faltan
 * reservas no es incompleta, es falsa.
 *
 * CUANDO SE LLAMA: una vez, al encender la medicion, ANTES de la primera
 * reserva y con un solo hilo corriendo.  No esta pensada para encenderse en
 * caliente -- escribe sobre codigo que otros hilos podrian estar ejecutando --,
 * que es por lo que una segunda llamada no hace nada.
 *
 * \~
 * @return
 * \~english false if some operator could not be patched: the system refused to
 *           make that page writable, or the thunk fell outside the reach of a
 *           relative jump.  THAT operator then keeps recording nothing, and
 *           whoever dumps the report has to say so rather than show a list that
 *           is quietly missing allocations.
 * \~spanish false si algun operador no se pudo parchear: el sistema se nego a
 *           hacer escribible esa pagina, o el trampolin cayo fuera del alcance
 *           de un salto relativo.  ESE operador se queda entonces sin apuntar
 *           nada, y quien vuelque el informe tiene que decirlo en vez de
 *           ensenar una lista a la que le faltan reservas en silencio.
 * \~
 */
bool install_call_site_patch() noexcept;

/**
 * @brief
 * \~english Was the patch installed?
 * \~spanish Se instalo el parche?
 * \~
 *
 * \~english
 * So the report can say whether the list is complete.  Without this, a patch
 * that failed would produce a shorter list that looks perfectly correct, which
 * is the worst way to be wrong.
 *
 * \~spanish
 * Para que el informe pueda decir si la lista esta completa.  Sin esto, un
 * parche que fallara produciria una lista mas corta con pinta de perfectamente
 * correcta, que es la peor forma de estar equivocado.
 *
 * \~
 * @return
 * \~english true when all eight operators were patched.
 * \~spanish true cuando se parchearon los ocho operadores.
 * \~
 */
bool call_site_patch_installed() noexcept;

} // namespace util

#endif // VESTA_UTIL_CALL_SITE_H

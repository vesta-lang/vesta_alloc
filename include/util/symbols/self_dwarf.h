/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file util/symbols/self_dwarf.h
 * @brief
 * \~english Which INLINED functions there are at an address of this process.
 * \~spanish Que funciones INLINEADAS hay en una direccion de este proceso.
 * \~
 *
 * \~english
 * THE PROBLEM IT SOLVES, and it is not a small one.
 * `util/symbols/self_symbols.h` answers "which function is this address in" by
 * looking at the symbol table, and it answers the truth: the function the
 * linker put there.  But with the optimiser on that function has eaten another
 * twenty, and the one of interest is almost never the outer one.
 *
 * Measured on this very compiler, a site that allocated came out as
 * `std::pair<...>::pair`, which is true and says nothing -- of course a `pair`
 * allocates.  The address had EIGHTEEN inlined frames, and the last one was:
 *
 *     __static_initialization_and_destruction_0
 *     include/emmit/parser_to_bytecode.h:103
 *
 * That IS the answer: the instruction table, built at start-up.  And it cannot
 * be got out of the stack -- the eighteen frames are inlined INSIDE ONE, so
 * there are no return addresses to walk.  It is only in the debug information.
 *
 * WHAT IT TAKES FOR IT TO WORK.  That the binary carries DWARF.  `-g1` is more
 * than enough: checked on this project, its 908 units at `-g1` give the whole
 * chain with file and line.  What `-g1` does not bring are the local variables,
 * which are not needed here.  Without DWARF -- the Release version -- this
 * returns zero frames, and whoever reports has to SAY SO.
 *
 * WHAT IT IS NOT.  It is not a debugger and not a stack unwinder: it reads no
 * variables, walks no frames and interprets no location expressions.  It
 * answers one question and only one, for a single address.
 *
 * COST.  Zero until somebody asks.  The first call builds the index of units
 * (some 280 KiB of `.debug_aranges`) and from then on every query reads off the
 * disk ONLY the unit that applies -- a few tens of KiB out of the 81 MiB
 * `.debug_info` takes.  A diagnostic prepared just in case is a cost everybody
 * pays so that nobody uses it.
 *
 * \~spanish
 * EL PROBLEMA QUE RESUELVE, y no es pequenyo.  `util/symbols/self_symbols.h` contesta
 * "de que funcion es esta direccion" mirando la tabla de simbolos, y contesta
 * la verdad: la funcion que el enlazador puso ahi.  Pero con el optimizador
 * encendido esa funcion se ha comido a otras veinte, y la que interesa casi
 * nunca es la de fuera.
 *
 * Medido en este propio compilador, un sitio que reservaba salia como
 * `std::pair<...>::pair`, que es cierto y no dice nada -- claro que un `pair`
 * reserva --.  La direccion tenia DIECIOCHO marcos inlineados, y el ultimo era:
 *
 *     __static_initialization_and_destruction_0
 *     include/emmit/parser_to_bytecode.h:103
 *
 * Eso si es la respuesta: la tabla de instrucciones, construida al arrancar.
 * Y no se puede sacar de la pila -- los dieciocho marcos estan inlineados
 * DENTRO DE UNO SOLO, asi que no hay direcciones de retorno que recorrer --.
 * Solo esta en la informacion de depuracion.
 *
 * QUE HACE FALTA PARA QUE FUNCIONE.  Que el binario lleve DWARF.  Con `-g1`
 * basta y sobra: comprobado en este proyecto, sus 908 unidades a `-g1` dan la
 * cadena entera con fichero y linea.  Lo que `-g1` no trae son las variables
 * locales, que aqui no hacen falta.  Sin DWARF -- la version de Release -- esto
 * devuelve cero marcos, y quien informe tiene que DECIRLO.
 *
 * QUE NO ES.  No es un depurador ni un desenrollador de pila: no lee variables,
 * no recorre marcos y no interpreta expresiones de localizacion.  Contesta una
 * pregunta y solo una, por una direccion suelta.
 *
 * COSTE.  Cero hasta que alguien pregunta.  La primera llamada construye el
 * indice de unidades (unos 280 KiB de `.debug_aranges`) y de ahi en adelante
 * cada consulta lee del disco SOLO la unidad que toca -- unas decenas de KiB de
 * los 81 MiB que ocupa `.debug_info` --.  Un diagnostico que se prepara por si
 * acaso es un coste que pagan todos para que lo use nadie.
 *
 * \~
 */

#ifndef VESTA_UTIL_SELF_DWARF_H
#define VESTA_UTIL_SELF_DWARF_H

#include <cstddef>

namespace util {

/**
 * @brief
 * \~english One frame of the chain: a function and where it was written.
 * \~spanish Un marco de la cadena: una funcion y donde estaba escrita.
 * \~
 *
 * \~english
 * The pointers live as long as the process -- they come out of a store that is
 * never released -- so they can be kept.
 *
 * \~spanish
 * Los punteros viven mientras el proceso -- salen de un almacen que no se
 * suelta -- asi que se pueden guardar.
 *
 * \~
 */
struct SelfFrame {
    /// \~english The name, not demangled.  Null when the unit does not say.
    /// \~spanish El nombre, sin desmanglar.  Nulo si la unidad no lo dice.
    /// \~
    const char *function;
    /// \~english The file, as the compiler wrote it.  Null when it is not
    ///           known.
    /// \~spanish El fichero, tal como lo escribio el compilador.  Nulo si no se
    ///           sabe.
    /// \~
    const char *file;
    /// \~english The line, or zero.  For an inlined frame it is WHERE IT WAS
    ///           CALLED FROM, which is exactly what is wanted; for the outer
    ///           one, where it was declared.
    /// \~spanish La linea, o cero.  Para un marco inlineado es DONDE SE LE
    ///           LLAMO, que es justo lo que se busca; para el de fuera, donde
    ///           se declaro.
    /// \~
    unsigned line;
    /// \~english true when this function was inlined inside the next one on the
    ///           list.
    /// \~spanish true si esta funcion se inlineo dentro de la siguiente de la
    ///           lista.
    /// \~
    bool inlined;
};

/**
 * @brief
 * \~english The chain of functions inlined at @p pc, from innermost to
 *          outermost.
 * \~spanish La cadena de funciones inlineadas en @p pc, de dentro hacia fuera.
 * \~
 *
 * \~english
 * The FIRST one is the innermost, where the code physically is; the LAST one is
 * the real function, the one that exists in the binary and has a symbol.  ALL
 * of them are returned: which one is the interesting one depends on who is
 * asking, and trimming to the one that looks useful here would be deciding for
 * them.
 *
 * @par Threads
 * Safe.  The first call builds the index and the rest only read.
 *
 * \~spanish
 * El PRIMERO es el mas interno, donde esta fisicamente el codigo; el ULTIMO es
 * la funcion de verdad, la que existe en el binario y tiene simbolo.  Se
 * devuelven TODOS: cual es el interesante depende de quien pregunte, y recortar
 * por el que parece util aqui seria decidir por el.
 *
 * @par Hilos
 * Segura.  La primera llamada construye el indice y las demas solo leen.
 *
 * \~
 * @param pc
 * \~english a code address of this process.
 * \~spanish una direccion de codigo de este proceso.
 * \~
 * @param out
 * \~english the caller's array.
 * \~spanish array del que llama.
 * \~
 * @param max
 * \~english how many fit.
 * \~spanish cuantos caben.
 * \~
 * @return
 * \~english how many were written.  Zero means it is not known -- no DWARF, or
 *           an address that is not this module's -- and it has to be said
 *           instead of showing an empty list, which reads as "there was
 *           nothing".
 * \~spanish cuantos se escribieron.  Cero significa que no se sabe -- sin
 *           DWARF, o una direccion que no es de este modulo --, y hay que
 *           decirlo en vez de ensenar una lista vacia, que se lee como "no
 *           habia nada".
 * \~
 *
 * \~english
 * @code
 *   util::SelfFrame f[32];
 *   const unsigned n = util::self_inline_frames(pc, f, 32);
 *   for (unsigned i = 0; i < n; ++i)
 *       std::printf("  %s%s  %s:%u\n", f[i].inlined ? "[inline] " : "",
 *                   f[i].function ? f[i].function : "?",
 *                   f[i].file ? f[i].file : "?", f[i].line);
 * @endcode
 *
 * \~spanish
 * @code
 *   util::SelfFrame f[32];
 *   const unsigned n = util::self_inline_frames(pc, f, 32);
 *   for (unsigned i = 0; i < n; ++i)
 *       std::printf("  %s%s  %s:%u\n", f[i].inlined ? "[inline] " : "",
 *                   f[i].function ? f[i].function : "?",
 *                   f[i].file ? f[i].file : "?", f[i].line);
 * @endcode
 *
 * \~
 */
unsigned self_inline_frames(const void *pc, SelfFrame *out,
                            unsigned max) noexcept;

/**
 * @brief
 * \~english Whether there is debug information FOR THIS address.
 * \~spanish Si hay informacion de depuracion PARA ESTA direccion.
 * \~
 *
 * \~english
 * WHY @c self_dwarf_units IS NOT ENOUGH.  A binary can carry DWARF for some
 * parts and not for others, and that is NORMAL: the libraries that come already
 * compiled -- the system's start-up one, third-party ones -- carry theirs,
 * while one's own code compiled without `-g` carries none.  Measured on this
 * project's Release version: forty-five units with information, and not one of
 * our code.
 *
 * Without this question, zero frames means two things that are nothing alike --
 * "nothing was inlined here" and "nothing is known about this" -- and whoever
 * reports cannot tell them apart.
 *
 * @par Threads
 * Safe.
 *
 * \~spanish
 * POR QUE NO BASTA CON @c self_dwarf_units.  Un binario puede traer DWARF de
 * unas partes y no de otras, y es lo NORMAL: las librerias que vienen ya
 * compiladas -- la de arranque del sistema, las de terceros -- llevan la suya,
 * mientras que el codigo propio compilado sin `-g` no lleva ninguna.  Medido en
 * la version de Release de este proyecto: cuarenta y cinco unidades con
 * informacion, y ni una de codigo nuestro.
 *
 * Sin esta pregunta, cero marcos significa dos cosas que no se parecen en nada
 * -- "aqui no se inlineo nada" y "de esto no se sabe nada" -- y quien informe
 * no puede distinguirlas.
 *
 * @par Hilos
 * Segura.
 *
 * \~
 * @param pc
 * \~english the address to ask about.
 * \~spanish la direccion por la que se pregunta.
 * \~
 * @return
 * \~english true when some unit covers it.
 * \~spanish true cuando alguna unidad la cubre.
 * \~
 */
bool self_dwarf_covers(const void *pc) noexcept;

/**
 * @brief
 * \~english How many compilation units the binary has.
 * \~spanish Cuantas unidades de compilacion tiene el binario.
 * \~
 *
 * \~english
 * It serves the same purpose as @c self_symbol_count: telling "there is no
 * information" from "there is nothing to count", which are two very different
 * answers and get written the same way if nobody separates them.
 *
 * \~spanish
 * Sirve para lo mismo que @c self_symbol_count: distinguir "no hay informacion"
 * de "no hay nada que contar", que son dos respuestas muy distintas y se
 * escriben igual si nadie las separa.
 *
 * \~
 * @return
 * \~english how many there are.  Zero = no DWARF.
 * \~spanish cuantas hay.  Cero = sin DWARF.
 * \~
 */
size_t self_dwarf_units() noexcept;

/**
 * @brief
 * \~english How many address RANGES the index covers.
 * \~spanish Cuantos TRAMOS de direcciones cubre el indice.
 * \~
 *
 * \~english
 * It is not the same as the number of units and the difference matters: with
 * `-ffunction-sections` a unit does not take one contiguous range but one per
 * function, so there are many more ranges than units.  And above all, units
 * with no ranges means the headers were read but their ranges were not
 * understood -- which looks the same as "there is no information" and is not.
 *
 * \~spanish
 * No es lo mismo que el numero de unidades y la diferencia importa: con
 * `-ffunction-sections` una unidad no ocupa un tramo seguido sino uno por
 * funcion, asi que hay muchos mas tramos que unidades.  Y sobre todo, unidades
 * sin tramos significa que se leyeron las cabeceras pero no se entendieron sus
 * rangos -- que se ve igual que "no hay informacion" y no lo es --.
 *
 * \~
 * @return
 * \~english how many ranges are in the index.
 * \~spanish cuantos tramos hay en el indice.
 * \~
 */
size_t self_dwarf_ranges() noexcept;

} // namespace util

#endif // VESTA_UTIL_SELF_DWARF_H

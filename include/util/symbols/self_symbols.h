/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file util/symbols/self_symbols.h
 * @brief
 * \~english Putting a NAME to a code address of THIS process.
 * \~spanish Ponerle NOMBRE a una direccion de codigo de ESTE proceso.
 * \~
 *
 * \~english
 * WHAT FOR.  Several parts of the system produce host addresses and cannot say
 * what they correspond to: the allocator's site table says a site allocated
 * four million times and can only give its offset.  An offset forces you out of
 * the program, to find the right binary and type `addr2line`, and that gets
 * done once and abandoned.  With the name inside, the diagnostic reads.
 *
 * WHY WE CAN.  Because the binary carries its COFF symbol table -- 132 thousand
 * entries in the Profile executable -- and this project already knows how to
 * read that format: its linker does it with every object.  Here the OWN
 * executable is read instead of somebody else's object, which is the same work.
 *
 * WHAT IT IS NOT.  It is not a stack unwinder and not a DWARF reader: it gives
 * no file and no line, it gives the function containing the address.  For the
 * exact file DWARF is still needed, and `--debug-info` is there for that.
 *
 * WHEN IT IS PAID FOR.  Never, until somebody asks.  The table is read off the
 * disk the FIRST time it is called and not before: it is a diagnostic, and a
 * diagnostic prepared just in case is a cost everybody pays so that nobody uses
 * it.
 *
 * \~spanish
 * PARA QUE.  Varias partes del sistema producen direcciones del anfitrion y no
 * saben decir a que corresponden: la tabla de sitios del asignador dice que un
 * sitio reservo cuatro millones de veces y solo puede dar su desplazamiento.
 * Un desplazamiento obliga a salir del programa, buscar el binario correcto y
 * teclear `addr2line`, y eso se hace una vez y se abandona.  Con el nombre
 * dentro, el diagnostico se lee.
 *
 * POR QUE PODEMOS.  Porque el binario lleva su tabla de simbolos COFF -- 132
 * mil entradas en el ejecutable de Profile --, y este proyecto ya sabe leer ese
 * formato: lo hace su enlazador con cada objeto.  Aqui se lee el ejecutable
 * PROPIO en vez de un objeto ajeno, que es el mismo trabajo.
 *
 * QUE NO ES.  No es un desenrollador de pila ni un lector de DWARF: no da
 * fichero ni linea, da la funcion que contiene la direccion.  Para el fichero
 * exacto sigue haciendo falta DWARF, y para eso ya esta `--debug-info`.
 *
 * CUANDO SE PAGA.  Nunca, hasta que alguien pregunta.  La tabla se lee del
 * disco la PRIMERA vez que se llama y no antes: es un diagnostico, y un
 * diagnostico que se prepara por si acaso es un coste que pagan todos para que
 * lo use nadie.
 *
 * \~
 */

#ifndef VESTA_UTIL_SELF_SYMBOLS_H
#define VESTA_UTIL_SELF_SYMBOLS_H

#include <cstddef>

namespace util {

/**
 * @brief
 * \~english The name of the function containing @p pc.
 * \~spanish El nombre de la funcion que contiene @p pc.
 * \~
 *
 * \~english
 * @par Threads
 * Safe.  The first call builds the table and the rest only read it; two threads
 * arriving at once both build one and one throws its own away, which is cheaper
 * than a lock on something that happens once.
 *
 * \~spanish
 * @par Hilos
 * Segura.  La primera llamada construye la tabla y las demas solo la leen; dos
 * hilos que lleguen a la vez construyen los dos y uno tira lo suyo, que es mas
 * barato que un cerrojo en algo que pasa una vez.
 *
 * \~
 * @param pc
 * \~english a code address of this process.
 * \~spanish una direccion de codigo de este proceso.
 * \~
 * @param offset
 * \~english if not null, receives how far it is from the start of the function
 *           to @p pc.  It serves to tell two sites inside the same function
 *           apart, which is exactly what happens with an `operator new` called
 *           from several points of a long method.
 * \~spanish si no es nulo, recibe cuanto hay desde el principio de la funcion
 *           hasta @p pc.  Sirve para distinguir dos sitios dentro de la misma
 *           funcion, que es exactamente lo que pasa con un `operator new`
 *           llamado desde varios puntos de un metodo largo.
 * \~
 * @return
 * \~english a pointer to text that lives as long as the process, or nullptr.
 *           It is NOT demangled: `_ZN...` comes out as it stands.  Demangling
 *           is another matter and whoever wants it can do it on top.
 * \~spanish un puntero a texto que vive mientras el proceso, o nullptr.  NO se
 *           desmangla: `_ZN...` sale tal cual.  Desmanglar es otra cosa y quien
 *           quiera puede hacerlo encima.
 * \~
 *
 * \~english
 * @code
 *   size_t off = 0;
 *   if (const char *n = util::self_symbol(pc, &off))
 *       std::printf("%s +%zu\n", n, off);
 *   else
 *       std::printf("%p (no symbol)\n", pc);
 * @endcode
 *
 * \~spanish
 * @code
 *   size_t off = 0;
 *   if (const char *n = util::self_symbol(pc, &off))
 *       std::printf("%s +%zu\n", n, off);
 *   else
 *       std::printf("%p (sin simbolo)\n", pc);
 * @endcode
 *
 * \~
 */
const char *self_symbol(const void *pc, size_t *offset = nullptr) noexcept;

/**
 * @brief
 * \~english The same, for a module that is not the one running.
 * \~spanish Lo mismo, para un modulo que no es el que esta corriendo.
 * \~
 *
 * \~english
 * WHY WHAT THE SYSTEM GIVES IS NOT ENOUGH.  Asking the loader whose an address
 * is returns the nearest EXPORTED symbol -- `dladdr` looks at `.dynsym`, and on
 * PE the export table is looked at -- and an internal function exports nothing:
 * there the answer is "no name" with the module's file right there, its whole
 * symbol table inside.  This reads it.
 *
 * And it is not a new reader: it is the SAME one that reads the binary itself.
 * The only thing that was particular to "oneself" -- where the path comes from
 * and which base the address is subtracted from -- now comes in as a parameter.
 *
 * @par Threads
 * Safe.  Each module's table is built once and is read only afterwards.
 *
 * \~spanish
 * POR QUE NO BASTA LO QUE DA EL SISTEMA.  Preguntarle al cargador de quien es
 * una direccion devuelve el simbolo EXPORTADO mas cercano -- `dladdr` mira
 * `.dynsym`, y en PE se mira la tabla de exportacion --, y una funcion interna
 * no exporta nada: ahi la respuesta es "no hay nombre" teniendo el fichero del
 * modulo delante, con su tabla de simbolos entera dentro.  Esto lo lee.
 *
 * Y no es un lector nuevo: es el MISMO que lee el binario propio.  Lo unico
 * que era propio de "uno mismo" -- de donde sale la ruta y contra que base se
 * resta la direccion -- entra ahora por parametro.
 *
 * @par Hilos
 * Segura.  La tabla de cada modulo se construye una vez y luego es de solo
 * lectura.
 *
 * \~
 * @param path
 * \~english the path of the module's file, as the system gives it.
 * \~spanish ruta del fichero del modulo, tal como la da el sistema.
 * \~
 * @param base
 * \~english where it is loaded; @p pc minus this is the offset.
 * \~spanish donde esta cargado; @p pc menos esto es el desplazamiento.
 * \~
 * @param pc
 * \~english the address to be named.
 * \~spanish la direccion que se quiere nombrar.
 * \~
 * @param offset
 * \~english how far it is from the symbol to @p pc.  A huge one means the
 *           symbol next to it is not the right one: without sizes there is no
 *           telling "inside" from "in the gap behind".
 * \~spanish cuanto hay del simbolo a @p pc.  Uno enorme significa que el
 *           simbolo de al lado no es el bueno: sin tamanos no se puede
 *           distinguir "dentro" de "en el hueco de detras".
 * \~
 * @return
 * \~english the name, or nullptr if the module has no table or it could not be
 *           read -- which is an answer, not a failure.
 * \~spanish el nombre, o nullptr si el modulo no tiene tabla o no se pudo leer
 *           -- que es una respuesta, no un fallo.
 * \~
 */
const char *module_symbol(const char *path, const void *base, const void *pc,
                          size_t *offset = nullptr) noexcept;

/**
 * @brief
 * \~english The EXACT range of the function containing @p pc.
 * \~spanish El tramo EXACTO de la funcion que contiene @p pc.
 * \~
 *
 * \~english
 * WHAT IT ADDS OVER @c self_symbol, which already gives a name and an offset.
 * A symbol says where a function STARTS and not where it ends, so the only
 * answer the symbol table can give is "the nearest previous symbol" -- correct
 * while the address falls inside, and wrong IN SILENCE as soon as it falls into
 * a gap or into a function that left no symbol.  With the range the two can be
 * checked against each other and it can be said which of them happened.
 *
 * AND IT ALSO SURVIVES STRIPPING.  On Windows the ranges come out of `.pdata`,
 * which is a SECTION -- the one the system uses to unwind the stack -- and not
 * a symbol table, so `--strip-all` does not take it: measured on this project,
 * the Release version keeps its 34,883 entries without a single symbol.  On
 * Linux they come out of each symbol's `st_size`, which ELF does keep.
 *
 * @par Threads
 * Safe, the same as @c self_symbol and with the same table.
 *
 * \~spanish
 * QUE ANADE SOBRE @c self_symbol, que ya da un nombre y un desplazamiento.
 * Un simbolo dice donde EMPIEZA una funcion y no donde acaba, asi que la unica
 * respuesta que puede dar la tabla de simbolos es "el simbolo anterior mas
 * cercano" -- correcto mientras la direccion caiga dentro, y equivocado EN
 * SILENCIO en cuanto cae en un hueco o en una funcion que no dejo simbolo.  Con
 * el tramo se puede contrastar y decir cual de las dos cosas pasa.
 *
 * Y ADEMAS SOBREVIVE AL DESPOJADO.  En Windows los tramos salen de `.pdata`,
 * que es una SECCION -- la que usa el sistema para desenrollar la pila -- y no
 * una tabla de simbolos, asi que `--strip-all` no se la lleva: medido sobre
 * este proyecto, la version de Release conserva sus 34.883 entradas sin tener
 * un solo simbolo.  En Linux salen del `st_size` de cada simbolo, que ELF si
 * guarda.
 *
 * @par Hilos
 * Segura, igual que @c self_symbol y con la misma tabla.
 *
 * \~
 * @param pc
 * \~english a code address of this process.
 * \~spanish una direccion de codigo de este proceso.
 * \~
 * @param begin
 * \~english if not null, receives the function's entry, as it is loaded.
 * \~spanish si no es nulo, recibe la entrada de la funcion, tal como esta
 *           cargada.
 * \~
 * @param size
 * \~english if not null, receives how big it is.
 * \~spanish si no es nulo, recibe cuanto mide.
 * \~
 * @return
 * \~english false if that address does not fall inside any known function --
 *           which is an answer and not a failure: there is padding between
 *           functions, and a leaf without unwind data has no record.
 * \~spanish false si esa direccion no cae dentro de ninguna funcion conocida --
 *           que es una respuesta y no un fallo: hay relleno entre funciones, y
 *           una hoja sin datos de desenrollado no tiene registro.
 * \~
 *
 * \~english
 * @code
 *   const void *fn = nullptr;
 *   size_t bytes = 0;
 *   if (util::self_function_range(pc, &fn, &bytes))
 *       std::printf("+%zu of %zu\n", size_t(uintptr_t(pc) - uintptr_t(fn)),
 *                   bytes);
 * @endcode
 *
 * \~spanish
 * @code
 *   const void *fn = nullptr;
 *   size_t bytes = 0;
 *   if (util::self_function_range(pc, &fn, &bytes))
 *       std::printf("+%zu de %zu\n", size_t(uintptr_t(pc) - uintptr_t(fn)),
 *                   bytes);
 * @endcode
 *
 * \~
 */
bool self_function_range(const void *pc, const void **begin = nullptr,
                         size_t *size = nullptr) noexcept;

/**
 * @brief
 * \~english How many function ranges could be read.  See
 *          @c self_function_range.
 * \~spanish Cuantos tramos de funcion se pudieron leer.  Ver
 *          @c self_function_range.
 * \~
 *
 * \~english
 * Zero means there are none, and whoever reports should SAY SO instead of
 * leaving the column empty -- the same as with the symbols.
 *
 * \~spanish
 * Cero significa que no hay ninguno, y quien informe deberia DECIRLO en vez de
 * dejar la columna vacia -- igual que con los simbolos.
 *
 * \~
 * @return
 * \~english how many were read.
 * \~spanish cuantos se leyeron.
 * \~
 */
size_t self_function_count() noexcept;

/**
 * @brief
 * \~english How many symbols could be read.
 * \~spanish Cuantos simbolos se pudieron leer.
 * \~
 *
 * \~english
 * Zero = it could not be done, and whoever asks should SAY SO instead of
 * showing a list without names as if there were no names to put.
 *
 * \~spanish
 * Cero = no se pudo, y quien lo pregunte deberia DECIRLO en vez de ensenar una
 * lista sin nombres como si no hubiera nombres que poner.
 *
 * \~
 * @return
 * \~english how many were read.
 * \~spanish cuantos se leyeron.
 * \~
 */
size_t self_symbol_count() noexcept;

} // namespace util

#endif // VESTA_UTIL_SELF_SYMBOLS_H

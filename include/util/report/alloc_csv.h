/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file util/report/alloc_csv.h
 * @brief
 * \~english The allocation report as DATA, for a tool to read.
 * \~spanish El informe de reservas como DATOS, para que lo lea una herramienta.
 * \~
 *
 * \~english
 * WHY, WITH A TEXT REPORT ALREADY THERE.  Because they answer different
 * questions.  The text one is read top to bottom and says what allocates most.
 * This one is for the question the text cannot answer: **where does an
 * allocation come from, and what path did it take** -- which means walking a
 * tree, folding branches, sorting by another column, going back.  That is a
 * tool's job, not a printout's.
 *
 * SEVERAL FILES, NOT ONE.  A site has many frames, so a single table would
 * repeat every site once per frame -- the same repetition that was taken out of
 * the text report.  They join on `site_id`:
 *
 *     sites.csv     one row per site: counts, bytes, purpose, shape, address
 *     frames.csv    one row per (site, depth): the inlining chain
 *     summary.csv   key/value: totals, region, what could not be resolved
 *     sizes.csv     the histogram of requested sizes, for the whole process
 *     site_sizes.csv the same histogram PER SITE, sparse.  A global one cannot
 *                   be handed back out to the sites that formed it, and
 *                   without this a branch of the tree can say WHICH sizes it
 *                   touches but not how many of each -- and a site with a
 *                   million 32-byte allocations looks like one that made a
 *                   single 16 MiB one.
 *     tags.csv      how much each declared purpose accounts for
 *
 * NAMES COME FROM OUTSIDE, FOR NOW.  This library measures; it does not read
 * symbol tables -- it ships on its own and has no business knowing about the
 * debug format of whoever links it.  So the naming enters through a HOOK that
 * the host installs (@c alloc_set_symbol_resolver).  Without one the CSVs still
 * come out, with offsets instead of names, and `summary.csv` says so rather
 * than leaving the columns blank.
 *
 * That split is temporary by design: when symbol resolution moves in here, the
 * only thing that changes is who fills the hook -- the files, the columns and
 * the tool stay exactly as they are.
 *
 * \~spanish
 * POR QUE, HABIENDO YA UN INFORME DE TEXTO.  Porque contestan preguntas
 * distintas.  El de texto se lee de arriba abajo y dice que reserva mas.  Este
 * es para la pregunta que el texto no puede contestar: **de donde sale una
 * reserva y por que camino llego** -- que significa recorrer un arbol, plegar
 * ramas, ordenar por otra columna, volver atras --.  Eso es trabajo de una
 * herramienta, no de un volcado.
 *
 * VARIOS FICHEROS, NO UNO.  Un sitio tiene muchos marcos, asi que una sola
 * tabla repetiria cada sitio una vez por marco -- la misma repeticion que se le
 * quito al informe de texto --.  Se unen por `site_id`:
 *
 *     sites.csv     una fila por sitio: cuentas, bytes, proposito, forma,
 *                   direccion
 *     frames.csv    una fila por (sitio, profundidad): la cadena de inlinado
 *     summary.csv   clave/valor: totales, region, lo que no se pudo resolver
 *     sizes.csv     el histograma de tamanos pedidos, del proceso entero
 *     site_sizes.csv el mismo histograma POR SITIO, disperso.  Uno global no se
 *                   puede repartir de vuelta entre los sitios que lo formaron,
 *                   y sin esto una rama del arbol puede decir QUE tamanos toca
 *                   pero no cuantas de cada uno -- y un sitio con un millon de
 *                   reservas de 32 bytes parece uno que hizo una sola de 16
 *                   MiB.
 *     tags.csv      cuanto se lleva cada proposito declarado
 *
 * LOS NOMBRES VIENEN DE FUERA, POR AHORA.  Esta libreria mide; no lee tablas de
 * simbolos -- se distribuye sola y no tiene por que saber del formato de
 * depuracion de quien la enlace --.  Asi que los nombres entran por un GANCHO
 * que instala el anfitrion (@c alloc_set_symbol_resolver).  Sin el, los CSV
 * salen igual, con desplazamientos en vez de nombres, y `summary.csv` lo dice
 * en vez de dejar las columnas en blanco.
 *
 * Esa division es temporal por diseno: cuando la resolucion de simbolos se mude
 * aqui dentro, lo unico que cambia es quien rellena el gancho -- los ficheros,
 * las columnas y la herramienta se quedan exactamente como estan.
 *
 * \~
 */

#ifndef VESTA_UTIL_ALLOC_CSV_H
#define VESTA_UTIL_ALLOC_CSV_H

#include <cstddef>

/* The shapes -- the frame and the two hooks -- are declared ONCE, in the C
 * header, and named again here.  Declaring them twice, one per language, is
 * the kind of duplication nobody notices going wrong: the day a field is added
 * to one of them the other keeps compiling and starts reading a different
 * struct. */
#include "util/report/alloc_csv_c.h"

namespace util {

/**
 * @brief
 * \~english One frame of the chain that leads to an allocation.
 * \~spanish Un marco de la cadena que lleva a una reserva.
 * \~
 *
 * \~english
 * Documented in `util/report/alloc_csv_c.h`, which is where it is declared.
 * The pointers must outlive the export call; the host owns them.  Anything it
 * does not know is left null (or zero), and that travels to the CSV as an empty
 * field -- which the tool shows as unknown, never as a name.
 *
 * \~spanish
 * Documentado en `util/report/alloc_csv_c.h`, que es donde se declara.  Los
 * punteros tienen que sobrevivir a la llamada de exportacion; son del
 * anfitrion.  Lo que no sepa se deja nulo (o a cero), y eso viaja al CSV como
 * un campo vacio -- que la herramienta ensena como desconocido, nunca como un
 * nombre.
 *
 * \~
 */
using AllocFrame = ::VestaAllocFrame;

/**
 * @brief
 * \~english What the host can tell about a code address.
 * \~spanish Lo que el anfitrion sepa decir de una direccion de codigo.
 * \~
 *
 * \~english
 * It takes the address this library recorded, an array of frames from innermost
 * to outermost and how many fit, and returns how many were written.  Zero means
 * "nothing is known about it", which is a legitimate answer and must not be
 * confused with an error.
 *
 * \~spanish
 * Recibe la direccion que apunto esta libreria, un array de marcos de dentro
 * hacia fuera y cuantos caben, y devuelve cuantos escribio.  Cero significa "no
 * se sabe nada de ella", que es una respuesta legitima y no hay que confundirla
 * con un error.
 *
 * \~
 */
using AllocSymbolResolver = ::VestaAllocSymbolResolver;

/**
 * @brief
 * \~english Turns a RAW symbol name into one a person reads.
 * \~spanish Convierte un nombre de simbolo EN CRUDO en uno que lee una persona.
 * \~
 *
 * \~english
 * WHY THIS IS A HOOK AND NOT A FUNCTION IN HERE.  Because mangling is a
 * property of the LANGUAGE and of its compiler, not of the allocator, and this
 * library is linked by projects that do not agree on it: C++ mangles one way,
 * Rust another, and a language that ships its own compiler -- Vesta among them
 * -- mangles as it pleases.  Building one demangler in would serve the first
 * and hand the rest a wall of `_ZNSt7__cxx11`.
 *
 * And it is not only about demangling: whoever installs this decides what
 * "readable" means for their code -- dropping the default template arguments,
 * calling a container by the name it has in the source, whatever their names
 * need.  The library asks the question; the answer is the project's.
 *
 * The pointer stays valid until the next call, like `strerror`: the caller
 * writes it out before asking again.  Keeping every name alive would mean
 * holding thousands of strings for a report that is written once.
 *
 * And it is not a C++ hook: a program written in C installs the same one
 * through `vesta_alloc_set_name_formatter`, and it is the only one that knows
 * which of its prefixes are noise.
 *
 * \~spanish
 * POR QUE ES UN GANCHO Y NO UNA FUNCION DE AQUI DENTRO.  Porque el decorado de
 * nombres es una propiedad del LENGUAJE y de su compilador, no del asignador, y
 * esta libreria la enlazan proyectos que no se ponen de acuerdo en ello: C++
 * decora de una forma, Rust de otra, y un lenguaje que trae su propio
 * compilador -- Vesta entre ellos -- decora como le parece.  Meter un
 * desdecorador dentro serviria al primero y le dejaria al resto un muro de
 * `_ZNSt7__cxx11`.
 *
 * Y no va solo de desdecorar: quien instala esto decide que significa
 * "legible" para su codigo -- quitar los argumentos de plantilla por defecto,
 * llamar a un contenedor por el nombre que tiene en el fuente, lo que sus
 * nombres necesiten --.  La libreria hace la pregunta; la respuesta es del
 * proyecto.
 *
 * El puntero vale hasta la llamada siguiente, como `strerror`: quien llama lo
 * escribe antes de volver a preguntar.  Mantener vivos todos los nombres
 * significaria guardar miles de cadenas para un informe que se escribe una vez.
 *
 * Y no es un gancho de C++: un programa escrito en C instala el mismo por
 * `vesta_alloc_set_name_formatter`, y es el unico que sabe cuales de sus
 * prefijos son ruido.
 *
 * \~
 */
using AllocNameFormatter = ::VestaAllocNameFormatter;

/**
 * @brief
 * \~english Installs the formatter.  Null removes it.
 * \~spanish Instala el formateador.  Nulo lo quita.
 * \~
 *
 * \~english
 * @par Threads
 * Set it once, at start-up, from the thread that will export.
 *
 * \~spanish
 * @par Hilos
 * Se pone una vez, al arrancar, desde el hilo que vaya a exportar.
 *
 * \~
 * @param fn
 * \~english what to call for every raw name, or null to leave them as they
 *           are.
 * \~spanish a que llamar con cada nombre en crudo, o nulo para dejarlos como
 *           estan.
 * \~
 */
void alloc_set_name_formatter(AllocNameFormatter fn) noexcept;

/**
 * @brief
 * \~english @p raw through the installed formatter, or @p raw itself.
 * \~spanish @p raw pasado por el formateador instalado, o @p raw tal cual.
 * \~
 *
 * \~english
 * It never returns null and never returns an empty string for a name that had
 * one: a formatter that cannot do anything with a name leaves it as it was,
 * which is ugly and TRUE -- and can still be pasted into whatever tool does
 * know how to read it.
 *
 * \~spanish
 * No devuelve nunca nulo ni una cadena vacia para un nombre que tenia uno: un
 * formateador que no sepa que hacer con un nombre lo deja como estaba, que es
 * feo y CIERTO -- y se puede pegar igualmente en la herramienta que si sepa
 * leerlo.
 *
 * \~
 * @param raw
 * \~english what the symbol table or the debug info says, verbatim.
 * \~spanish lo que diga la tabla de simbolos o la informacion de depuracion,
 *           literal.
 * \~
 * @return
 * \~english the readable form, valid until the next call.
 * \~spanish la forma legible, valida hasta la llamada siguiente.
 * \~
 */
const char *alloc_readable_name(const char *raw) noexcept;

/**
 * @brief
 * \~english Installs the resolver.  Null removes it.
 * \~spanish Instala el resolutor.  Nulo lo quita.
 * \~
 *
 * \~english
 * Call it before exporting; there is no point installing it later.  It is a
 * plain pointer with no locking: this is set once, during start-up, by the same
 * thread that will later export.
 *
 * \~spanish
 * Se llama antes de exportar; instalarlo despues no sirve de nada.  Es un
 * puntero a secas y sin cerrojo: esto se pone una vez, al arrancar, desde el
 * mismo hilo que exportara luego.
 *
 * \~
 * @param fn
 * \~english what to ask about each address, or null to go back to offsets.
 * \~spanish a que preguntar por cada direccion, o nulo para volver a los
 *           desplazamientos.
 * \~
 */
void alloc_set_symbol_resolver(AllocSymbolResolver fn) noexcept;

/**
 * @brief
 * \~english The resolver that is installed, or null.
 * \~spanish El resolutor que este instalado, o nulo.
 * \~
 *
 * \~english
 * WHY ANYBODY WOULD ASK.  Because the CSV is not the only place a site gets
 * written down: the exit report prints its top sites to the terminal too, and
 * it used to print BARE OFFSETS while the CSV of the very same process printed
 * `parse_tokens`.  The same measurement in two qualities, and the poorer one is
 * what somebody sees who does not know the export exists.
 *
 * So the text dump asks for the resolver as well.  It stays a HOOK rather than
 * a hard dependency: this library must still be usable by somebody who has no
 * symbols to offer, and then the offset is what there is.
 *
 * \~spanish
 * POR QUE PREGUNTARIA NADIE.  Porque el CSV no es el unico sitio donde se
 * apunta un sitio: el informe de salida imprime tambien sus primeros sitios en
 * el terminal, y antes imprimia DESPLAZAMIENTOS PELADOS mientras el CSV del
 * mismisimo proceso imprimia `parse_tokens`.  La misma medicion en dos
 * calidades, y la pobre es la que ve quien no sabe que la exportacion existe.
 *
 * Asi que el volcado de texto pregunta tambien por el resolutor.  Sigue siendo
 * un GANCHO y no una dependencia dura: esta libreria tiene que seguir siendo
 * utilizable por alguien que no tenga simbolos que ofrecer, y entonces el
 * desplazamiento es lo que hay.
 *
 * \~
 * @return
 * \~english the installed resolver, or null when there is none.
 * \~spanish el resolutor instalado, o nulo si no hay ninguno.
 * \~
 */
AllocSymbolResolver alloc_symbol_resolver() noexcept;

/**
 * @brief
 * \~english Writes the CSVs into @p dir.
 * \~spanish Escribe los CSV en @p dir.
 * \~
 *
 * \~english
 * @par Threads
 * Not safe against allocation from other threads: it takes a snapshot and reads
 * it.  It is meant to run at the end, when there is nothing else going on.
 *
 * \~spanish
 * @par Hilos
 * No es segura contra reservas de otros hilos: toma una foto y la lee.  Esta
 * pensada para correr al final, cuando ya no pasa nada mas.
 *
 * \~
 * @param dir
 * \~english where to put them.  It is CREATED if it is not there, with every
 *           level it is missing -- asking the caller to prepare a directory
 *           only buys one failure mode, and a bad one: the run ends, the report
 *           is gone, and all that is left is a line saying a folder was
 *           missing.  Whoever asked for a report wants the report.  Empty means
 *           the current directory.
 * \~spanish donde ponerlos.  Se CREA si no esta, con todos los niveles que le
 *           falten -- pedirle al que llama que prepare un directorio solo
 *           compra un modo de fallo, y malo: la corrida termina, el informe no
 *           esta, y lo unico que queda es una linea diciendo que faltaba una
 *           carpeta --.  Quien pidio un informe quiere el informe.  Vacio
 *           significa el directorio actual.
 * \~
 * @return
 * \~english false if any of them could not be written, having said which on
 *           stderr.  A report that half exists is worse than none: the tool
 *           would load it and show a tree with holes that look like real data.
 * \~spanish false si alguno no se pudo escribir, habiendo dicho cual por
 *           stderr.  Un informe que existe a medias es peor que ninguno: la
 *           herramienta lo cargaria y ensenaria un arbol con huecos que parecen
 *           datos de verdad.
 * \~
 *
 * \~english
 * @code
 *   if (const char *dir = getenv("VESTA_HOST_ALLOC_CSV"))
 *       util::write_alloc_csv(dir);
 * @endcode
 *
 * \~spanish
 * @code
 *   if (const char *dir = getenv("VESTA_HOST_ALLOC_CSV"))
 *       util::write_alloc_csv(dir);
 * @endcode
 *
 * \~
 */
bool write_alloc_csv(const char *dir);

} // namespace util

#endif // VESTA_UTIL_ALLOC_CSV_H

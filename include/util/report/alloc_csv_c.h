/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file util/report/alloc_csv_c.h
 * @brief
 * \~english The allocation report, with the signatures C understands.
 * \~spanish El informe de reservas, con las firmas que entiende C.
 * \~
 *
 * \~english
 * WHY IT EXISTS.  Because the report is not a C++ feature and must not read
 * like one.  The libraries carried inside this project are written in C --
 * Capstone, SQLite, OpenSSL, miniz -- and they already allocate through
 * `util/alloc/host_allocator_c.h`, so their memory is counted.  Being counted and
 * being REPORTABLE are different things: a program written in C that links
 * this allocator has to be able to ask for the export, and to say what a name
 * of ITS language looks like, without one line of C++.
 *
 * And the naming hook is exactly where that matters.  A C project has no
 * mangling to undo, but it does have `static` functions with the same name in
 * ten files, macro-generated symbols and prefixes to trim -- and it is the
 * only one that knows which.  Handing it a C++-only hook would be telling it
 * to live with whatever the symbol table happens to say.
 *
 * ONE DEFINITION, NOT TWO.  The frame struct and the two hook types are
 * declared HERE and the C++ header aliases them.  Declaring them twice -- once
 * per language -- is the kind of duplication nobody notices going wrong: the
 * day a field is added to one of them, the other keeps compiling and starts
 * reading a different struct.
 *
 * THIS HEADER IS PURE C.  No templates, no references, no namespaces, and it
 * includes no C++ header.  It can be included from a `.c` file, and it is: see
 * `examples/c_report.c`, which is compiled as C precisely so that this stays
 * true.
 *
 * \~spanish
 * POR QUE EXISTE.  Porque el informe no es una capacidad de C++ y no debe
 * leerse como si lo fuera.  Las librerias que este proyecto lleva dentro estan
 * escritas en C -- Capstone, SQLite, OpenSSL, miniz -- y ya reservan por
 * `util/alloc/host_allocator_c.h`, asi que su memoria se cuenta.  Que se cuente
 * y que se pueda INFORMAR son cosas distintas: un programa escrito en C que
 * enlace este asignador tiene que poder pedir la exportacion, y decir que
 * pinta tiene un nombre de SU lenguaje, sin una sola linea de C++.
 *
 * Y el gancho de los nombres es justo donde eso importa.  Un proyecto en C no
 * tiene decorado que deshacer, pero si tiene funciones `static` con el mismo
 * nombre en diez ficheros, simbolos generados por macros y prefijos que
 * recortar -- y es el unico que sabe cuales --.  Darle un gancho que solo
 * exista en C++ seria decirle que se conforme con lo que ponga la tabla de
 * simbolos.
 *
 * UNA DEFINICION, NO DOS.  La estructura del marco y los dos tipos de gancho se
 * declaran AQUI y la cabecera de C++ les pone alias.  Declararlos dos veces --
 * uno por lenguaje -- es la duplicacion que nadie ve romperse: el dia que se le
 * anade un campo a uno, el otro sigue compilando y empieza a leer una
 * estructura distinta.
 *
 * ESTA CABECERA ES C PURO.  Nada de plantillas, referencias ni namespaces, y no
 * incluye ninguna cabecera de C++.  Se puede incluir desde un `.c`, y se hace:
 * ver `examples/c_report.c`, que se compila como C precisamente para que esto
 * siga siendo cierto.
 *
 * \~
 *
 * \~english
 * @code
 *   // The whole thing, from C.
 *   static const char *my_names(const char *raw) {
 *       return raw[0] == '_' ? raw + 1 : raw;   // trim our own prefix
 *   }
 *   vesta_alloc_set_name_formatter(my_names);
 *   vesta_alloc_write_csv("/tmp/run");
 * @endcode
 *
 * \~spanish
 * @code
 *   // Todo, desde C.
 *   static const char *mis_nombres(const char *raw) {
 *       return raw[0] == '_' ? raw + 1 : raw;   // recortar nuestro prefijo
 *   }
 *   vesta_alloc_set_name_formatter(mis_nombres);
 *   vesta_alloc_write_csv("/tmp/corrida");
 * @endcode
 *
 * \~
 */
#ifndef VESTA_UTIL_ALLOC_CSV_C_H
#define VESTA_UTIL_ALLOC_CSV_C_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief
 * \~english One frame of the chain that leads to an allocation.
 * \~spanish Un marco de la cadena que lleva a una reserva.
 * \~
 *
 * \~english
 * The pointers must outlive the export call; whoever fills them owns them.
 * Anything not known is left NULL (or zero), and that travels to the CSV as an
 * empty field -- which the tool shows as unknown, never as a name.
 *
 * \~spanish
 * Los punteros tienen que sobrevivir a la llamada de exportacion; son de quien
 * los rellena.  Lo que no se sepa se deja a NULL (o a cero), y eso viaja al CSV
 * como un campo vacio -- que la herramienta ensena como desconocido, nunca como
 * un nombre.
 *
 * \~
 */
typedef struct VestaAllocFrame {
    /**
     * \~english mangled or plain; the tool does not care
     * \~spanish decorado o no; a la herramienta le da igual
     * \~
     */
    const char *function;
    /**
     * \~english the source file, or NULL when it is not known
     * \~spanish el fichero fuente, o NULL si no se sabe
     * \~
     */
    const char *file;
    /**
     * \~english the line in that file; zero when it is not known
     * \~spanish la linea de ese fichero; cero si no se sabe
     * \~
     */
    unsigned line;
    /**
     * \~english
     * Non-zero when this function was inlined into the NEXT one in the chain.
     * An `int` and not a boolean so that the struct means the same thing in
     * C89, in C99 and in C++ without anyone having to include anything.
     *
     * \~spanish
     * Distinto de cero cuando esta funcion se metio dentro de la SIGUIENTE de
     * la cadena.  Un `int` y no un booleano para que la estructura signifique
     * lo mismo en C89, en C99 y en C++ sin que nadie tenga que incluir nada.
     *
     * \~
     */
    int inlined;
    /**
     * @brief
     * \~english Whose code this is: the standard library, the start-up files, a
     *          third-party library, or which module of the project's own.
     * \~spanish De quien es este codigo: la biblioteca estandar, los ficheros
     *          de arranque, una libreria de terceros, o que modulo del propio
     *          proyecto.
     * \~
     *
     * \~english
     * WHY WHOEVER FILLS THIS ANSWERS IT AND NOT US.  Because "who allocates"
     * has two useful answers and they are very different.  One is the exact
     * function, which the fields above already give.  The other is whose code
     * it is -- and with four hundred rows of standard-library templates mixed
     * in with the project's own, that one is invisible.  But the mapping from
     * a path to a module is a property of the PROJECT's layout: here a module
     * is a directory under `src/`, elsewhere it is a package, a crate or a
     * bundle.  Deciding it in here would mean guessing at someone else's tree.
     *
     * NULL when not classified, which the tool shows as unknown.
     *
     * \~spanish
     * POR QUE CONTESTA ESTO QUIEN LO RELLENA Y NO NOSOTROS.  Porque "quien
     * reserva" tiene dos respuestas utiles y son muy distintas.  Una es la
     * funcion exacta, que ya dan los campos de arriba.  La otra es de quien es
     * el codigo -- y con cuatrocientas filas de plantillas de la biblioteca
     * estandar mezcladas con las del propio proyecto, esa es invisible --.
     * Pero el paso de una ruta a un modulo es una propiedad de como esta puesto
     * el PROYECTO: aqui un modulo es un directorio bajo `src/`, en otro sitio
     * es un paquete, un crate o un bundle.  Decidirlo aqui dentro seria adivinar
     * en el arbol de otro.
     *
     * NULL si no se clasifico, que la herramienta ensena como desconocido.
     *
     * \~
     */
    const char *module;
} VestaAllocFrame;

/**
 * @brief
 * \~english What the caller can tell about a code address.
 * \~spanish Lo que quien llama sepa decir de una direccion de codigo.
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
typedef unsigned (*VestaAllocSymbolResolver)(const void *pc,
                                             VestaAllocFrame *out,
                                             unsigned max);

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
 * Rust another, a language that ships its own compiler mangles as it pleases,
 * and C does not mangle at all.  Building one demangler in would serve the
 * first and hand the rest a wall of `_ZNSt7__cxx11`.
 *
 * It takes what the symbol table or the debug info says, verbatim, and returns
 * the readable form or NULL to keep the raw one.  The pointer stays valid until
 * the next call, like `strerror`: the caller writes it out before asking again.
 *
 * \~spanish
 * POR QUE ES UN GANCHO Y NO UNA FUNCION DE AQUI DENTRO.  Porque el decorado de
 * nombres es una propiedad del LENGUAJE y de su compilador, no del asignador, y
 * esta libreria la enlazan proyectos que no se ponen de acuerdo en ello: C++
 * decora de una forma, Rust de otra, un lenguaje que trae su propio compilador
 * decora como le parece, y C no decora nada.  Meter un desdecorador dentro
 * serviria al primero y le dejaria al resto un muro de `_ZNSt7__cxx11`.
 *
 * Recibe lo que diga la tabla de simbolos o la informacion de depuracion,
 * literal, y devuelve la forma legible o NULL para quedarse con la cruda.  El
 * puntero vale hasta la llamada siguiente, como `strerror`: quien llama lo
 * escribe antes de volver a preguntar.
 *
 * \~
 */
typedef const char *(*VestaAllocNameFormatter)(const char *raw);

/**
 * @brief
 * \~english Installs the resolver.  NULL removes it.
 * \~spanish Instala el resolutor.  NULL lo quita.
 * \~
 *
 * \~english
 * @par Threads
 * Set it once, at start-up, from the thread that will export.  It is a plain
 * pointer with no locking.
 *
 * \~spanish
 * @par Hilos
 * Se pone una vez, al arrancar, desde el hilo que vaya a exportar.  Es un
 * puntero a secas y sin cerrojo.
 *
 * \~
 * @param fn
 * \~english what to ask about each address, or NULL to go back to offsets.
 * \~spanish a que preguntar por cada direccion, o NULL para volver a los
 *           desplazamientos.
 * \~
 */
void vesta_alloc_set_symbol_resolver(VestaAllocSymbolResolver fn);

/**
 * @brief
 * \~english Installs the name formatter.  NULL removes it.
 * \~spanish Instala el formateador de nombres.  NULL lo quita.
 * \~
 *
 * \~english
 * @par Threads
 * Same as above: once, at start-up.
 *
 * \~spanish
 * @par Hilos
 * Igual que la de arriba: una vez, al arrancar.
 *
 * \~
 * @param fn
 * \~english what to call for every raw name, or NULL to leave them as they
 *           are.
 * \~spanish a que llamar con cada nombre en crudo, o NULL para dejarlos como
 *           estan.
 * \~
 */
void vesta_alloc_set_name_formatter(VestaAllocNameFormatter fn);

/**
 * @brief
 * \~english @p raw through the installed formatter, or @p raw itself.
 * \~spanish @p raw pasado por el formateador instalado, o @p raw tal cual.
 * \~
 *
 * \~english
 * It never returns NULL and never returns an empty string for a name that had
 * one: a formatter that cannot do anything with a name leaves it as it was,
 * which is ugly and TRUE -- and can still be pasted into whatever tool does
 * know how to read it.
 *
 * \~spanish
 * No devuelve nunca NULL ni una cadena vacia para un nombre que tenia uno: un
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
const char *vesta_alloc_readable_name(const char *raw);

/**
 * @brief
 * \~english Writes the CSVs into @p dir.
 * \~spanish Escribe los CSV en @p dir.
 * \~
 *
 * \~english
 * @par Threads
 * Not safe against allocation from other threads: it takes a snapshot and
 * reads it.  It is meant to run at the end, when there is nothing else going
 * on.
 *
 * \~spanish
 * @par Hilos
 * No es segura contra reservas de otros hilos: toma una foto y la lee.  Esta
 * pensada para correr al final, cuando ya no pasa nada mas.
 *
 * \~
 * @param dir
 * \~english where to put them.  It is CREATED if it is not there, with every
 *           level it is missing.  Empty means the current directory.
 * \~spanish donde ponerlos.  Se CREA si no esta, con todos los niveles que le
 *           falten.  Vacio significa el directorio actual.
 * \~
 * @return
 * \~english non-zero on success; zero if any of them could not be written,
 *           having said which on stderr.  A report that half exists is worse
 *           than none: the tool would load it and show a tree with holes that
 *           look like real data.
 * \~spanish distinto de cero si salio bien; cero si alguno no se pudo escribir,
 *           habiendo dicho cual por stderr.  Un informe que existe a medias es
 *           peor que ninguno: la herramienta lo cargaria y ensenaria un arbol
 *           con huecos que parecen datos de verdad.
 * \~
 *
 * \~english
 * @code
 *   const char *dir = getenv("VESTA_HOST_ALLOC_CSV");
 *   if (dir != NULL) vesta_alloc_write_csv(dir);
 * @endcode
 *
 * \~spanish
 * @code
 *   const char *dir = getenv("VESTA_HOST_ALLOC_CSV");
 *   if (dir != NULL) vesta_alloc_write_csv(dir);
 * @endcode
 *
 * \~
 */
int vesta_alloc_write_csv(const char *dir);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VESTA_UTIL_ALLOC_CSV_C_H */

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
 * \~english Says WHOSE code a frame is, given where its source lives.
 * \~spanish Dice DE QUIEN es el codigo de un marco, a partir de donde vive su
 *           fuente.
 * \~
 *
 * \~english
 * WHY THIS IS A HOOK AND NOT A RULE IN HERE.  Because "which module is this"
 * is a property of the CONSUMER's tree and of nothing else: one project keeps
 * its subsystems under `src/<name>/`, another one module per directory, another
 * vendors its dependencies somewhere this library has never heard of.  Any
 * rule written in here would be right for exactly one project and would go on
 * answering -- wrongly -- for every other, which is worse than not answering.
 *
 * It is asked only for frames the resolver left WITHOUT a module, which is the
 * consumer's own code: a frame in somebody else's binary already carries the
 * module it came from.  Returning NULL means "I do not know", and that is a
 * legitimate answer -- the column comes out empty, not guessed.
 *
 * The pointer stays valid until the next call, like `VestaAllocNameFormatter`
 * and for the same reason: the caller writes it out before asking again, so a
 * classifier can hold one buffer and not allocate while the report is running.
 *
 * @par The file path, and how to know where your tree is
 * The path is whatever the debug information holds, which is the shape the
 * compiler saw -- so it is the same shape as `__FILE__` in the consumer's own
 * sources.  That is the cheap way to find your own root without writing it
 * down: take `__FILE__` of one of your files, strip the part you know follows
 * the root, and what is left is the prefix every source of yours starts with.
 * A path that does not start with it is not yours, however much it looks like
 * it -- the compiler's own runtime has a `/src/` in it too.
 *
 * \~spanish
 * POR QUE ES UN GANCHO Y NO UNA REGLA DE AQUI DENTRO.  Porque "de que modulo es
 * esto" es una propiedad del arbol del CONSUMIDOR y de nada mas: un proyecto
 * guarda sus subsistemas en `src/<nombre>/`, otro un modulo por directorio,
 * otro mete sus dependencias en un sitio del que esta libreria no ha oido
 * hablar.  Cualquier regla escrita aqui acertaria en exactamente un proyecto y
 * seguiria contestando -- mal -- en todos los demas, que es peor que no
 * contestar.
 *
 * Se pregunta solo por los marcos que el resolutor dejo SIN modulo, que son los
 * del codigo propio del consumidor: un marco de un binario ajeno ya lleva el
 * modulo del que salio.  Devolver NULL es "no lo se", y es una respuesta
 * legitima -- la columna sale vacia, no adivinada.
 *
 * El puntero vale hasta la llamada siguiente, como `VestaAllocNameFormatter` y
 * por lo mismo: quien llama lo escribe antes de volver a preguntar, asi que un
 * clasificador puede tener un solo bufer y no reservar mientras corre el
 * informe.
 *
 * @par La ruta del fichero, y como saber donde esta tu arbol
 * La ruta es la que lleve la informacion de depuracion, que es la forma que vio
 * el compilador -- o sea la misma forma que `__FILE__` en los fuentes del
 * consumidor.  Esa es la manera barata de encontrar tu propia raiz sin
 * escribirla: coger `__FILE__` de uno de tus ficheros, quitarle la parte que
 * sabes que va detras de la raiz, y lo que queda es el prefijo con el que
 * empiezan todos tus fuentes.  Una ruta que no empiece por ahi no es tuya por
 * mucho que lo parezca -- el runtime del propio compilador tambien lleva un
 * `/src/` dentro.
 *
 * \~
 * @param file
 * \~english the source of the frame, or NULL when there is none.
 * \~spanish el fuente del marco, o NULL si no hay.
 * \~
 * @param function
 * \~english its name, for when there is no path to go by.
 * \~spanish su nombre, para cuando no hay ruta de la que tirar.
 * \~
 * @return
 * \~english the module, or NULL for "I do not know".
 * \~spanish el modulo, o NULL para "no lo se".
 * \~
 */
typedef const char *(*VestaAllocModuleClassifier)(const char *file,
                                                  const char *function);

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
 * \~english Installs the module classifier.  NULL removes it.
 * \~spanish Instala el clasificador de modulos.  NULL lo quita.
 * \~
 *
 * \~english
 * Without one, the module column carries what the resolver knew -- which for a
 * frame of the consumer's own code is usually nothing -- and the report can
 * group by file and by function but not by module.  That is a smaller answer,
 * never a wrong one.
 *
 * @par Threads
 * Same as the others: once, at start-up, from the thread that will export.
 *
 * \~spanish
 * Sin el, la columna de modulo lleva lo que supiera el resolutor -- que para un
 * marco del codigo propio del consumidor suele ser nada -- y el informe puede
 * agrupar por fichero y por funcion pero no por modulo.  Eso es una respuesta
 * mas corta, nunca una equivocada.
 *
 * @par Hilos
 * Como los demas: una vez, al arrancar, desde el hilo que vaya a exportar.
 *
 * \~
 * @param fn
 * \~english what to ask about each source path, or NULL for no modules.
 * \~spanish a que preguntar por cada ruta de fuente, o NULL para no tener
 *           modulos.
 * \~
 */
void vesta_alloc_set_module_classifier(VestaAllocModuleClassifier fn);

/**
 * @brief
 * \~english Where the consumer's tree starts, worked out from @c __FILE__.
 * \~spanish Donde empieza el arbol del consumidor, deducido de @c __FILE__.
 * \~
 *
 * \~english
 * A classifier needs to know which paths are its own, and the cheapest true
 * answer is `__FILE__`: it expands to the path the COMPILER saw, which is the
 * same shape the debug information holds.  Strip from it the part the caller
 * knows -- where that file sits in its own tree -- and what is left is the
 * prefix every source of that project starts with.  Nothing to keep up to date
 * and nothing to write twice: move the tree, rename the checkout, build on
 * another machine, and it still answers.
 *
 * WHY IT IS A FUNCTION HERE AND NOT THREE LINES IN EVERY CONSUMER.  Because
 * `__FILE__` on Windows arrives with BACKSLASHES and the relative path is
 * written with forward ones, so the obvious `rfind` finds nothing and the
 * classifier answers "I do not know" for everything -- silently, which is the
 * worst way to be wrong.  That was written twice here and it was wrong twice.
 *
 * `__FILE__` is expanded at the point it is WRITTEN, so it has to be passed in
 * by the caller: taken inside this library it would name a file of this
 * library, which is never what anybody wants.  @c VESTA_ALLOC_ROOT_HERE does
 * exactly that and is the way to call it.
 *
 * \~spanish
 * Un clasificador necesita saber que rutas son suyas, y la respuesta cierta mas
 * barata es `__FILE__`: se expande a la ruta que vio el COMPILADOR, que es la
 * misma forma que lleva la informacion de depuracion.  Quitandole la parte que
 * quien llama sabe -- donde esta ese fichero dentro de su propio arbol -- queda
 * el prefijo con el que empiezan todos los fuentes de ese proyecto.  Nada que
 * mantener y nada que escribir dos veces: mueve el arbol, renombra la copia,
 * construye en otra maquina, y sigue contestando.
 *
 * POR QUE ES UNA FUNCION DE AQUI Y NO TRES LINEAS EN CADA CONSUMIDOR.  Porque
 * `__FILE__` en Windows llega con BARRAS INVERTIDAS y la ruta relativa se
 * escribe con normales, asi que el `rfind` evidente no encuentra nada y el
 * clasificador contesta "no lo se" para todo -- en silencio, que es la peor
 * forma de equivocarse.  Se escribio dos veces aqui y las dos salio mal.
 *
 * `__FILE__` se expande donde se ESCRIBE, asi que lo tiene que pasar quien
 * llama: cogido dentro de esta libreria nombraria un fichero de esta libreria,
 * que no es lo que quiere nadie.  @c VESTA_ALLOC_ROOT_HERE hace justo eso y es
 * la forma de llamarla.
 *
 * \~
 * @param file_macro
 * \~english what @c __FILE__ expanded to where the caller wrote it.
 * \~spanish a lo que se expandio @c __FILE__ donde lo escribio quien llama.
 * \~
 * @param relative
 * \~english where that same file sits in the tree, with forward slashes.
 * \~spanish donde esta ese mismo fichero en el arbol, con barras normales.
 * \~
 * @return
 * \~english the root, with forward slashes and a trailing one, or NULL if the
 *          two do not fit together.  Valid until the next call.
 * \~spanish la raiz, con barras normales y una al final, o NULL si los dos no
 *          encajan.  Vale hasta la llamada siguiente.
 * \~
 */
const char *vesta_alloc_root_of(const char *file_macro, const char *relative);

/**
 * \~english The root of the file this is WRITTEN in.  @p rel is where that file
 *           sits in the tree: `VESTA_ALLOC_ROOT_HERE("src/report.c")`.
 * \~spanish La raiz del fichero donde esto se ESCRIBE.  @p rel es donde esta
 *           ese fichero en el arbol: `VESTA_ALLOC_ROOT_HERE("src/report.c")`.
 * \~
 */
#define VESTA_ALLOC_ROOT_HERE(rel) vesta_alloc_root_of(__FILE__, (rel))

/**
 * @brief
 * \~english Says that everything under @p dir is module @p name.
 * \~spanish Dice que todo lo que cuelga de @p dir es el modulo @p name.
 * \~
 *
 * \~english
 * DECLARED BEATS DEDUCED, and that is the whole point: a rule over paths is a
 * guess that keeps answering after the tree moves, and it answers WRONG without
 * anybody noticing.  What is declared here is checked against nothing and
 * guesses nothing -- the code says what it is.
 *
 * The LONGEST directory wins, so a subsystem inside another one declares itself
 * and everything else keeps inheriting: `src/ir` and `src/ir/regalloc` can both
 * be modules without either having to know about the other.
 *
 * What is NOT declared is not lost: the installed classifier still gets asked
 * (see @c VestaAllocModuleClassifier), so a project can declare the parts that
 * matter and leave the rest to whatever rule it had.
 *
 * @par Threads and allocation
 * Meant to be called before `main`, from the registrars the macros below
 * create.  It writes into a fixed table and allocates nothing -- it runs inside
 * the program being measured, so it may not disturb the very thing it measures.
 * Past the table's size it says so once and drops the rest, which is the same
 * contract as everything else in here: a limit that is reached out loud.
 *
 * \~spanish
 * LO DECLARADO GANA A LO DEDUCIDO, y de eso va todo esto: una regla sobre rutas
 * es una suposicion que sigue contestando cuando el arbol se mueve, y contesta
 * MAL sin que nadie se entere.  Lo que se declara aqui no se comprueba contra
 * nada ni adivina nada -- lo dice el codigo.
 *
 * Gana el directorio MAS LARGO, asi que un subsistema dentro de otro se declara
 * y el resto sigue heredando: `src/ir` y `src/ir/regalloc` pueden ser dos
 * modulos sin que ninguno tenga que saber del otro.
 *
 * Lo que NO se declara no se pierde: al clasificador instalado se le sigue
 * preguntando (ver @c VestaAllocModuleClassifier), asi que un proyecto puede
 * declarar lo que le importa y dejar el resto a la regla que tuviera.
 *
 * @par Hilos y reservas
 * Pensada para llamarse antes de `main`, desde los registradores que crean las
 * macros de abajo.  Escribe en una tabla fija y no reserva nada -- corre dentro
 * del programa que se esta midiendo, asi que no puede molestar justo a lo que
 * mide.  Pasado el tamano de la tabla lo dice una vez y descarta el resto, que
 * es el mismo contrato que todo lo de aqui: un limite que se alcanza en voz
 * alta.
 *
 * \~
 * @param dir
 * \~english the directory, as the compiler sees it.  Slashes either way.
 * \~spanish el directorio, tal como lo ve el compilador.  Barras de cualquier
 *           tipo.
 * \~
 * @param name
 * \~english what to call it.  It must outlive the report -- a literal.
 * \~spanish como se llama.  Tiene que vivir mas que el informe -- un literal.
 * \~
 */
void vesta_alloc_declare_module(const char *dir, const char *name);

/**
 * @brief
 * \~english A stretch of CODE and whose it is.
 * \~spanish Un tramo de CODIGO y de quien es.
 * \~
 *
 * \~english
 * WHY BY ADDRESS AND NOT BY PATH.  Because in a release build there is no path:
 * measured on this project, a stripped binary gives 1.174 frames and NOT ONE of
 * them carries a file -- so every rule over source paths, declared or deduced,
 * answers nothing.  The address is the only thing that is always there.
 *
 * WHERE THE CONSUMER GETS THESE.  Wherever it likes, and that is the point: a
 * map file from the linker, its own debug information read on its own terms, a
 * table generated at build time, the exports of its own modules.  This library
 * neither reads nor guesses -- it is handed the answer.
 *
 * @par A size of zero
 * Means "until the next declared stretch", which is how a MARKER works: a
 * consumer that can only say "this function is somewhere inside module X" says
 * exactly that, and the ranges fall out of the order.  It is an approximation
 * and the report says so, rather than passing it off as measured.
 *
 * \~spanish
 * POR QUE POR DIRECCION Y NO POR RUTA.  Porque en una construccion de
 * distribucion no hay ruta: medido en este proyecto, un binario estripado da
 * 1.174 marcos y NI UNO lleva fichero -- asi que toda regla sobre rutas de
 * fuente, declarada o deducida, no contesta nada.  La direccion es lo unico que
 * esta siempre.
 *
 * DE DONDE LAS SACA EL CONSUMIDOR.  De donde quiera, y de eso se trata: un
 * fichero de mapa del enlazador, su propia informacion de depuracion leida como
 * el sepa, una tabla generada al construir, los exportados de sus modulos.
 * Esta libreria ni lee ni adivina -- se le da la respuesta.
 *
 * @par Un tamano de cero
 * Significa "hasta el tramo declarado siguiente", que es como funciona un
 * MARCADOR: un consumidor que solo sabe decir "esta funcion esta en algun sitio
 * dentro del modulo X" dice justo eso, y los rangos salen del orden.  Es una
 * aproximacion y el informe lo dice, en vez de colarla como una medida.
 *
 * \~
 */
typedef struct VestaAllocCodeRange {
    /**
     * \~english where the stretch starts.
     * \~spanish donde empieza el tramo.
     * \~
     */
    const void *addr;
    /**
     * \~english how long it is, or 0 for "until the next one" -- see above.
     * \~spanish cuanto ocupa, o 0 para "hasta el siguiente" -- ver arriba.
     * \~
     */
    size_t size;
    /**
     * \~english the symbol, or NULL when only the module is known.
     * \~spanish el simbolo, o NULL cuando solo se sabe el modulo.
     * \~
     */
    const char *name;
    /**
     * \~english whose it is, or NULL when only the name is known.
     * \~spanish de quien es, o NULL cuando solo se sabe el nombre.
     * \~
     */
    const char *module;
} VestaAllocCodeRange;

/**
 * @brief
 * \~english Hands over a table of code stretches.  It is NOT copied.
 * \~spanish Entrega una tabla de tramos de codigo.  NO se copia.
 * \~
 *
 * \~english
 * The array and every string in it belong to the caller and must outlive the
 * report -- the same contract as the frames a resolver fills in, and for the
 * same reason: copying a symbol table of a hundred thousand entries inside the
 * allocator would put the report into the very measurement it is taking.
 *
 * SORTED BY ADDRESS, ascending.  It is checked once, and an unsorted table is
 * still used -- by walking it whole -- and SAID, because a table that quietly
 * answers wrong is worse than a slow one.  It can be called several times, one
 * per module or per shared object.
 *
 * \~spanish
 * El array y cada cadena de dentro son de quien llama y tienen que vivir mas
 * que el informe -- el mismo contrato que los marcos que rellena un resolutor,
 * y por lo mismo: copiar dentro del asignador una tabla de simbolos de cien mil
 * entradas meteria el informe dentro de la medida que esta tomando.
 *
 * ORDENADA POR DIRECCION, de menor a mayor.  Se comprueba una vez, y una tabla
 * desordenada se usa igual -- recorriendola entera -- y se DICE, porque una
 * tabla que contesta mal en silencio es peor que una lenta.  Se puede llamar
 * varias veces, una por modulo o por biblioteca.
 *
 * \~
 * @param ranges
 * \~english the table, which stays the caller's.
 * \~spanish la tabla, que sigue siendo de quien llama.
 * \~
 * @param count
 * \~english how many entries.
 * \~spanish cuantas entradas.
 * \~
 */
void vesta_alloc_declare_code(const VestaAllocCodeRange *ranges,
                              unsigned count);

/**
 * @brief
 * \~english Says that the code around @p pc was compiled from @p file.
 * \~spanish Dice que el codigo de alrededor de @p pc se compilo de @p file.
 * \~
 *
 * \~english
 * WHY A SINGLE ADDRESS AND NOT A RANGE.  Because a translation unit cannot know
 * where its own code ends: the compiler splits it into one section per function
 * and the linker places them wherever it likes.  What a file CAN say is "this
 * address is mine", and the ranges fall out of the order of all the markers --
 * which is an approximation, and the report calls it `nearby` so that nobody
 * reads it as a measurement.
 *
 * WHAT IT BUYS over declaring the module: the FILE.  A module says which
 * subsystem allocated; this says which of its two hundred files, which is the
 * difference between "the IR allocates a lot" and "it is the emitter".  Where
 * there is a linker map this is redundant -- the map gives the same file,
 * exactly -- so this is for what the map cannot reach: a binary built without
 * one, or copied away from its `.map`.
 *
 * \~spanish
 * POR QUE UNA SOLA DIRECCION Y NO UN RANGO.  Porque una unidad de compilacion
 * no puede saber donde acaba su propio codigo: el compilador lo parte en una
 * seccion por funcion y el enlazador las coloca donde le parece.  Lo que un
 * fichero SI puede decir es "esta direccion es mia", y los rangos salen del
 * orden de todos los marcadores -- que es una aproximacion, y el informe la
 * llama `nearby` para que nadie la lea como una medida.
 *
 * QUE APORTA sobre declarar el modulo: el FICHERO.  Un modulo dice que
 * subsistema reservo; esto dice cual de sus doscientos ficheros, que es la
 * diferencia entre "el IR reserva mucho" y "es el emisor".  Donde hay mapa del
 * enlazador esto sobra -- el mapa da el mismo fichero, exacto --, asi que es
 * para lo que el mapa no alcanza: un binario construido sin el, o separado de
 * su `.map`.
 *
 * \~
 * @param pc
 * \~english any address inside the file's own code.
 * \~spanish una direccion cualquiera del codigo del propio fichero.
 * \~
 * @param file
 * \~english what to call it.  It must outlive the report -- a literal.
 * \~spanish como se llama.  Tiene que vivir mas que el informe -- un literal.
 * \~
 */
void vesta_alloc_declare_file(const void *pc, const char *file);

/**
 * \~english ONE LINE in a file, to say which file its code is.  The name comes
 *           from `__FILE__` and the address from a function this plants, so
 *           nothing is written by hand.  At file scope.
 * \~spanish UNA LINEA en un fichero, para decir de que fichero es su codigo.
 *           El nombre sale de `__FILE__` y la direccion de una funcion que esto
 *           mismo pone, asi que no se escribe nada a mano.  A nivel de fichero.
 * \~
 */
#if !defined(VESTA_ALLOC_LINKED)
#define VESTA_ALLOC_FILE_HERE() static_assert(true, "")
#elif defined(__cplusplus)
#define VESTA_ALLOC_FILE_HERE()                                                \
    namespace {                                                                \
    /* Una funcion de ESTA unidad, que es lo unico que da una direccion suya.   \
       No hace nada: lo que importa es donde ESTA. */                          \
    void vesta_alloc_file_anchor() {}                                          \
    struct VestaAllocFileRegistrar {                                           \
        VestaAllocFileRegistrar() {                                            \
            vesta_alloc_declare_file(                                          \
                reinterpret_cast<const void *>(&vesta_alloc_file_anchor),      \
                __FILE__);                                                     \
        }                                                                      \
    };                                                                         \
    const VestaAllocFileRegistrar vesta_alloc_file_registrar;                  \
    }                                                                          \
    static_assert(true, "")
#elif defined(__GNUC__) || defined(__clang__)
#define VESTA_ALLOC_FILE_HERE()                                                \
    static void vesta_alloc_file_anchor(void) {}                               \
    __attribute__((constructor)) static void vesta_alloc_declare_file_here(    \
        void) {                                                                \
        /* Por una union: ver `VESTA_ALLOC_FILE_TU`. */                        \
        union { void (*fn)(void); const void *p; } vesta_alloc_cast;           \
        vesta_alloc_cast.fn = &vesta_alloc_file_anchor;                        \
        vesta_alloc_declare_file(vesta_alloc_cast.p, __FILE__);                \
    }                                                                          \
    extern int vesta_alloc_file_here_semicolon
#else
#define VESTA_ALLOC_FILE_HERE()                                                \
    /* sin constructores: llamar a vesta_alloc_declare_file al arrancar */
#endif

/**
 * \~english The same, but naming the TRANSLATION UNIT instead of the file this
 *           is written in -- which is what makes it work from a header that the
 *           build forces into every source (`-include`).
 *
 * WHY IT MATTERS.  A convention that has to be written in seven hundred files
 * is a convention that gets forgotten in the seven hundred and first, and a
 * file that forgets it does not go missing: its allocations land on the
 * PREVIOUS file's marker, with its name on them.  Forced by the build, no file
 * can forget, and a new one is covered the day it is added.
 *
 * `__BASE_FILE__` is a GNU extension -- GCC and Clang have it -- and it is the
 * whole trick: inside a header, `__FILE__` is the header, and `__BASE_FILE__`
 * is the `.cpp` being compiled.  Where it does not exist this falls back to
 * `__FILE__`, which from a header would declare the header once per unit: no
 * use, but no lie either -- every unit would claim the same name and the report
 * shows it as such.
 *
 * \~spanish La misma, pero nombrando la UNIDAD DE COMPILACION en vez del
 *           fichero donde se escribe -- que es lo que permite ponerla en una
 *           cabecera que el build fuerza en todos los fuentes (`-include`).
 *
 * POR QUE IMPORTA.  Una convencion que hay que escribir en setecientos ficheros
 * es una convencion que se olvida en el setecientos uno, y un fichero que se
 * olvida no desaparece: sus reservas caen en el marcador del fichero ANTERIOR,
 * con su nombre puesto.  Forzada por el build no se le olvida a nadie, y un
 * fichero nuevo queda cubierto el dia que se anade.
 *
 * `__BASE_FILE__` es una extension de GNU -- GCC y Clang la tienen -- y es todo
 * el truco: dentro de una cabecera, `__FILE__` es la cabecera y `__BASE_FILE__`
 * es el `.cpp` que se esta compilando.  Donde no exista se cae a `__FILE__`,
 * que desde una cabecera declararia la cabecera una vez por unidad: no sirve,
 * pero tampoco miente -- todas dirian el mismo nombre y el informe lo ensena.
 * \~
 */
#if defined(__GNUC__) || defined(__clang__)
#define VESTA_ALLOC_TU_NAME __BASE_FILE__
#else
#define VESTA_ALLOC_TU_NAME __FILE__
#endif

#if !defined(VESTA_ALLOC_LINKED)
#define VESTA_ALLOC_FILE_TU() static_assert(true, "")
#elif defined(__cplusplus)
#define VESTA_ALLOC_FILE_TU()                                                  \
    namespace {                                                                \
    void vesta_alloc_tu_anchor() {}                                            \
    struct VestaAllocTuRegistrar {                                             \
        VestaAllocTuRegistrar() {                                              \
            vesta_alloc_declare_file(                                          \
                reinterpret_cast<const void *>(&vesta_alloc_tu_anchor),        \
                VESTA_ALLOC_TU_NAME);                                          \
        }                                                                      \
    };                                                                         \
    const VestaAllocTuRegistrar vesta_alloc_tu_registrar;                      \
    }                                                                          \
    static_assert(true, "")
#elif defined(__GNUC__) || defined(__clang__)
#define VESTA_ALLOC_FILE_TU()                                                  \
    static void vesta_alloc_tu_anchor(void) {}                                 \
    __attribute__((constructor)) static void vesta_alloc_declare_tu(void) {    \
        /* Por una UNION y no con un cast: ISO C prohibe convertir un puntero  \
           a funcion en uno a objeto, y con `-Wpedantic` eso no es un aviso,   \
           es un error.  La union es el idioma portable para lo mismo. */      \
        union { void (*fn)(void); const void *p; } vesta_alloc_cast;           \
        vesta_alloc_cast.fn = &vesta_alloc_tu_anchor;                          \
        vesta_alloc_declare_file(vesta_alloc_cast.p, VESTA_ALLOC_TU_NAME);     \
    }                                                                          \
    extern int vesta_alloc_file_tu_semicolon
#else
#define VESTA_ALLOC_FILE_TU() /* sin constructores: ver arriba */
#endif

/**
 * @brief
 * \~english The same, taking a FILE of that module: its directory is the one
 *          declared.
 * \~spanish Lo mismo, dandole un FICHERO de ese modulo: se declara su
 *          directorio.
 * \~
 *
 * \~english
 * This is what @c VESTA_ALLOC_MODULE_HERE calls with `__FILE__`, so a module
 * declares itself without anybody writing a path: the compiler already knows
 * where the file is, and that is exactly the shape the debug information holds.
 *
 * \~spanish
 * Es lo que llama @c VESTA_ALLOC_MODULE_HERE con `__FILE__`, para que un modulo
 * se declare sin que nadie escriba una ruta: el compilador ya sabe donde esta
 * el fichero, y esa es justo la forma que lleva la informacion de depuracion.
 *
 * \~
 * @param file_macro
 * \~english what @c __FILE__ expanded to.
 * \~spanish a lo que se expandio @c __FILE__.
 * \~
 * @param name
 * \~english the module's name.
 * \~spanish el nombre del modulo.
 * \~
 */
void vesta_alloc_declare_module_of_file(const char *file_macro,
                                        const char *name);

/**
 * \~english DECLARING COSTS NOTHING AT LINK TIME, and weak symbols are NOT how.
 *
 * A source file that declares its module gets compiled into more than one
 * binary: the program, a unit test that pulls in three files, an archive that
 * may only depend on libc.  Not all of them link this library, and a plain
 * reference turns a declaration -- which is documentation about the code --
 * into a link error in whichever target forgot it.
 *
 * WEAK WAS TRIED AND IT CRASHES ON PE.  An undefined weak symbol is supposed to
 * resolve to zero so the caller can check it.  With MinGW it resolves to the
 * IMAGE BASE instead, so `fn != 0` is true, the constructor calls it, and the
 * process jumps to the top of its own image and dies before `main` -- measured,
 * with no output at all, which is as hard to read as a crash gets.
 *
 * So the gate is at COMPILE time: the declaration exists only where this
 * library is linked, and that is stated by linking it -- the `vesta_alloc`
 * target defines `VESTA_ALLOC_LINKED` for whoever links it.  A file keeps its
 * declaration in the source, where it belongs, and compiles to nothing in a
 * binary that has no report to declare anything to.
 *
 * \~spanish DECLARAR NO CUESTA NADA AL ENLAZAR, y los simbolos debiles NO son
 * la forma.
 *
 * Un fichero que declara su modulo se compila en mas de un binario: el
 * programa, un test que se lleva tres ficheros, un archivo que solo puede
 * depender de libc.  No todos enlazan esta libreria, y una referencia normal
 * convierte una declaracion -- que es documentacion sobre el codigo -- en un
 * error de enlace en el objetivo que no la tenga.
 *
 * SE PROBO CON DEBILES Y REVIENTA EN PE.  Un simbolo debil sin definir deberia
 * valer cero para que quien llama lo compruebe.  Con MinGW vale la BASE DE LA
 * IMAGEN, asi que `fn != 0` es cierto, el constructor lo llama, y el proceso
 * salta al principio de su propia imagen y muere antes de `main` -- medido, sin
 * una linea de salida, que es lo mas dificil de leer que puede ser una caida.
 *
 * Asi que la puerta esta en COMPILACION: la declaracion solo existe donde esta
 * libreria se enlaza, y eso se dice enlazandola -- el objetivo `vesta_alloc`
 * define `VESTA_ALLOC_LINKED` para quien lo haga.  Un fichero conserva su
 * declaracion en el fuente, que es donde va, y compila a nada en un binario que
 * no tiene informe al que declararle nada.
 * \~
 */

/**
 * \~english ONE LINE, in any file of the module: everything beside it and under
 *           it is @p name.  No path is written -- `__FILE__` says where this
 *           is, which is why it has to be written HERE and not in the library.
 *           At file scope, once per module.
 * \~spanish UNA LINEA, en cualquier fichero del modulo: todo lo que esta a su
 *           lado y debajo es @p name.  No se escribe ninguna ruta -- `__FILE__`
 *           dice donde esta esto, que es la razon de que haya que escribirlo
 *           AQUI y no en la libreria.  A nivel de fichero, una vez por modulo.
 * \~
 */
#if !defined(VESTA_ALLOC_LINKED)
/* Sin la libreria enlazada, declarar no genera codigo: ver la nota de arriba.
   La linea se queda en el fuente, que es donde dice algo. */
#define VESTA_ALLOC_MODULE_HERE(name) static_assert(true, "")
#elif defined(__cplusplus)
#define VESTA_ALLOC_MODULE_HERE(name)                                          \
    namespace {                                                                \
    struct VestaAllocModuleRegistrar {                                         \
        VestaAllocModuleRegistrar() {                                          \
            vesta_alloc_declare_module_of_file(__FILE__, (name));              \
        }                                                                      \
    };                                                                         \
    const VestaAllocModuleRegistrar vesta_alloc_module_registrar;              \
    }                                                                          \
    /* SE COME EL PUNTO Y COMA de quien la escribe.  Sin esto, ponerlo -- que   \
       es lo que el cuerpo pide -- es un `;` de mas y con `-Wpedantic` no       \
       compila; y no ponerlo se olvida siempre. */                             \
    static_assert(true, "")
#elif defined(__GNUC__) || defined(__clang__)
#define VESTA_ALLOC_MODULE_HERE(name)                                          \
    __attribute__((constructor)) static void vesta_alloc_declare_here(void) {  \
        vesta_alloc_declare_module_of_file(__FILE__, (name));                  \
    }                                                                          \
    /* Y en C, una declaracion que nadie define: se come el `;` y no genera     \
       codigo. */                                                              \
    extern int vesta_alloc_module_here_semicolon
#else
/* Sin constructores, la declaracion se hace a mano desde el arranque del
 * programa.  Se define vacia y se DICE, en vez de que la macro parezca que
 * funciona y el modulo no salga en ningun sitio. */
#define VESTA_ALLOC_MODULE_HERE(name)                                          \
    /* este compilador no tiene constructores: llama a                         \
       vesta_alloc_declare_module_of_file(__FILE__, name) al arrancar */
#endif

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

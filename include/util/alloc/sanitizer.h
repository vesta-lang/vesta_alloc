/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file util/alloc/sanitizer.h
 * @brief
 * \~english The CHECKING mode: leaks, double frees and foreign pointers, named.
 * \~spanish El modo COMPROBACION: fugas, dobles liberaciones y punteros
 *           ajenos, con nombre.
 * \~
 *
 * \~english
 * MEASURING AND CHECKING ARE TWO MODES, AND ONLY ONE MAY NOT PERTURB.  The
 * allocator refuses to put a header in front of each block, and the reason is
 * good: 81% of allocations are 64 bytes or less, so 16 extra bytes move a
 * 48-byte request into the 64 class -- other classes, other chunks, other cache
 * -- and the instrumented run would measure ANOTHER PROGRAM.
 *
 * That rule belongs to the MEASURING mode, where the number is the product.
 * This is the CHECKING mode: it produces verdicts, not numbers, so perturbing
 * is fine -- nothing here is being timed.  Confusing the two is what makes
 * people conclude "we cannot do that" when the truth is "the measuring mode
 * cannot do that".
 *
 * WHY IT IS OURS AND NOT AN EXISTING TOOL.  Because valgrind is Linux-only and
 * there is no usable equivalent on Windows with this toolchain, so pointing at
 * an external tool leaves HALF the platforms unable to look at their own
 * memory.  And it is cheap here because the expensive parts already exist: the
 * allocation doors are already interposed on both systems, symbols already
 * resolve to `file:line` through our own DWARF reader and PE export table --
 * no external symbolizer -- and page permissions are already ours to change.
 *
 * OVERFLOWS ARE CAUGHT TWO WAYS, and which one depends on the level.  With the
 * size classes, a canary behind the caller's bytes catches it WHEN THE BLOCK IS
 * RELEASED -- late, but for the price of four bytes.  With @c SanLevel::Guard
 * each block gets pages of its own and is placed FLUSH AGAINST a guard page
 * that is not mapped, so one byte past the end faults AT THE INSTANT OF THE
 * WRITE, with the real address, and no compiler pass anywhere.
 *
 * WHAT IS LEFT, said up front so nobody expects it: a wild write that jumps
 * clean over the guard page and lands in some other mapping.  Catching that
 * needs a check inserted at every load and store, which means a compiler pass
 * over the host C++, and there is none.  And, because a block can only be flush
 * against ONE edge of its pages, the guard catches overflow or underflow but
 * not both at once -- so which edge it guards is a knob.
 *
 * IT COSTS NOTHING WHEN IT IS OFF, AND THAT IS A COMPILE-TIME PROMISE.  A
 * run-time gate would cost one load and one branch per allocation to do
 * nothing, forever, in every build.  So the switch is a MACRO, exactly like
 * @c VESTA_ALLOC_SIZE_HISTOGRAM: with it off, every entry point below is an
 * empty inline function and the generated code of `host_alloc` and `host_free`
 * is identical, instruction for instruction, to a build where this file does
 * not exist.
 *
 * The LEVEL, on the other hand, is chosen at run time -- inside a build that
 * already carries the checker.  There a gate costs nothing worth counting,
 * because that build does not exist to go fast, and being able to dial the cost
 * down without recompiling is what makes the checker get used at all.
 *
 * THE RULE THAT GOVERNS THIS FILE: THE CHECKER ADAPTS.  Where a design of the
 * allocator gets in the checker's way, the allocator is RIGHT and stays as it
 * is; the checker solves it on its own side.  The canary changes which size
 * class is picked -- only in this build.  A shrinking `realloc` moves the end
 * of the block -- the checker refreshes its own canary there.  The aligned door
 * keeps the original pointer just before the block -- the checker takes that
 * into account.  The bootstrap arena and the loader's blocks are not leaks --
 * the checker recognises them.  If a check ever required bending the allocator,
 * the right answer is that the check does not go in.
 *
 * \~spanish
 * MEDIR Y COMPROBAR SON DOS MODOS, Y SOLO UNO TIENE PROHIBIDO PERTURBAR.  El
 * asignador se niega a poner una cabecera delante de cada bloque, y la razon es
 * buena: el 81% de las reservas son de 64 bytes o menos, asi que 16 bytes de
 * mas mueven una peticion de 48 a la clase de 64 -- otras clases, otros trozos,
 * otra cache -- y la corrida instrumentada mediria OTRO PROGRAMA.
 *
 * Esa regla es del modo MEDIDA, donde el numero es el producto.  Este es el
 * modo COMPROBACION: produce veredictos, no numeros, asi que perturbar da igual
 * -- aqui no se cronometra nada --.  Confundir los dos es lo que lleva a
 * concluir "eso no se puede hacer" cuando lo cierto es "eso no lo puede hacer
 * el modo medida".
 *
 * POR QUE ES NUESTRO Y NO UNA HERRAMIENTA QUE YA EXISTE.  Porque valgrind es de
 * Linux y en Windows con este juego de herramientas no hay equivalente usable,
 * asi que remitir a la herramienta externa deja la MITAD de las plataformas sin
 * poder mirar su propia memoria.  Y aqui sale barato porque lo caro ya esta:
 * las puertas de reserva estan interpuestas en los dos sistemas, los simbolos
 * ya se resuelven a `fichero:linea` con nuestro lector DWARF y la tabla de
 * exportacion de PE -- sin simbolizador externo -- y los permisos de pagina ya
 * son nuestros.
 *
 * LOS DESBORDAMIENTOS SE CAZAN DE DOS FORMAS, y cual depende del nivel.  Con
 * las clases de tamano, un canario detras de los bytes del llamante lo caza AL
 * SOLTAR EL BLOQUE -- tarde, pero por cuatro bytes --.  Con @c SanLevel::Guard
 * cada bloque tiene paginas propias y se coloca PEGADO a una pagina de guarda
 * sin mapear, asi que un byte mas alla del final falla EN EL INSTANTE DE LA
 * ESCRITURA, con la direccion de verdad, y sin ningun pase de compilador.
 *
 * LO QUE QUEDA FUERA, dicho por delante para que nadie lo espere: una escritura
 * perdida que salte limpiamente por encima de la pagina de guarda y caiga en
 * otro mapeo.  Cazar eso si pide una comprobacion en cada carga y cada almacen,
 * o sea un pase de compilador sobre el C++ del anfitrion, y no lo hay.  Y, como
 * un bloque solo puede quedar pegado a UN borde de sus paginas, la guarda caza
 * el desbordamiento o el subdesbordamiento, no los dos a la vez -- asi que que
 * borde se guarda es un ajuste mas.
 *
 * NO CUESTA NADA APAGADO, Y ESO ES UNA PROMESA DE COMPILACION.  Un portillo en
 * ejecucion costaria una carga y un salto por reserva para no hacer nada, para
 * siempre y en todos los builds.  Asi que el interruptor es una MACRO, igual
 * que @c VESTA_ALLOC_SIZE_HISTOGRAM: apagada, cada entrada de abajo es una
 * funcion en linea vacia y el codigo generado de `host_alloc` y `host_free` es
 * identico, instruccion a instruccion, al de un build donde este fichero no
 * existe.
 *
 * El NIVEL, en cambio, se elige en ejecucion -- dentro de un build que ya lleva
 * el comprobador dentro.  Ahi un portillo no cuesta nada que merezca contarse,
 * porque ese build no existe para ir rapido, y poder bajar el coste sin
 * recompilar es lo que hace que el comprobador se llegue a usar.
 *
 * LA REGLA QUE GOBIERNA ESTE FICHERO: EL COMPROBADOR SE ADAPTA.  Donde un
 * diseno del asignador le estorbe, el asignador tiene RAZON y se queda como
 * esta; el comprobador lo resuelve en su lado.  El canario cambia que clase se
 * elige -- solo en este build --.  Un `realloc` que encoge mueve el final del
 * bloque -- el comprobador refresca alli su canario --.  La puerta alineada
 * guarda el puntero original justo antes -- el comprobador lo tiene en cuenta
 * --.  La arena de arranque y los bloques del cargador no son fugas -- el
 * comprobador los reconoce --.  Si alguna comprobacion llegara a exigir doblar
 * el asignador, la respuesta correcta es que esa comprobacion no entra.
 *
 * \~
 *
 * @code
 *   cmake -DVESTA_ALLOC_SANITIZER=ON ...
 *   VESTA_ALLOC_SAN=2 ./my_program          # shadow + double free + canary
 * @endcode
 */
#ifndef VESTA_ALLOC_SANITIZER_H
#define VESTA_ALLOC_SANITIZER_H

#include <cstddef>
#include <cstdint>

namespace util {

#if defined(VESTA_ALLOC_SANITIZER) && VESTA_ALLOC_SANITIZER

/**
 * @brief
 * \~english How much checking is switched on, chosen at run time.
 * \~spanish Cuanta comprobacion esta encendida, elegido en ejecucion.
 * \~
 *
 * \~english
 * The levels are cumulative and their costs are of different ORDERS, which is
 * the whole reason they are separate: with a single on/off the choice is
 * between everything and nothing, and nothing wins.
 *
 * Set with `VESTA_ALLOC_SAN`; the default is @c Track, which is the one that
 * pays for itself.
 *
 * \~spanish
 * Los niveles son acumulativos y sus costes son de ORDENES distintos, que es
 * justo por lo que estan separados: con un solo encendido/apagado se elige
 * entre todo o nada, y gana nada.
 *
 * Se pone con `VESTA_ALLOC_SAN`; por defecto @c Track, que es el que se paga
 * solo.
 * \~
 */
enum class SanLevel : unsigned {
    /// \~english Nothing.  The code is compiled in but does not run.
    /// \~spanish Nada.  El codigo esta compilado pero no corre.
    /// \~
    Off = 0,
    /// \~english Shadow: leaks, double frees, foreign pointers, and which
    ///           thread allocated versus which one released.
    /// \~spanish Sombreado: fugas, dobles liberaciones, punteros ajenos, y que
    ///           hilo reservo frente a cual solto.
    /// \~
    Track = 1,
    /// \~english Adds the canary, so an overflow is caught when the block is
    ///           released.
    /// \~spanish Anade el canario, con lo que un desbordamiento se caza al
    ///           soltar el bloque.
    /// \~
    Canary = 2,
    /// \~english Adds poisoning on release, so a WRITE after free is caught
    ///           when the block is handed out again.
    /// \~spanish Anade envenenar al soltar, con lo que una ESCRITURA despues de
    ///           liberar se caza al volver a entregar el bloque.
    /// \~
    Poison = 3,
    /**
     * @brief
     * \~english Pages of its own per block, flush against a guard page.
     * \~spanish Paginas propias por bloque, pegado a una pagina de guarda.
     * \~
     *
     * \~english
     * The only level that catches things AT THE INSTANT: a write past the end
     * hits a page that is not mapped and faults right there, and a released
     * block has its pages decommitted so touching it faults too.  No canary to
     * read later and no poison to check on reuse -- the fault IS the report,
     * with the real address and the real stack.
     *
     * IT IS EXPENSIVE ON PURPOSE and it is nobody's default: a sixteen-byte
     * allocation costs a page plus a guard page, address space runs out fast
     * and every block is a TLB entry of its own.  It exists to be pointed at a
     * program that is already known to be broken.
     *
     * \~spanish
     * El unico nivel que caza cosas EN EL INSTANTE: una escritura pasado el
     * final da en una pagina sin mapear y falla ahi mismo, y un bloque soltado
     * tiene sus paginas descomprometidas, asi que tocarlo tambien falla.  Ni
     * canario que leer despues ni veneno que comprobar al reutilizar -- el
     * fallo ES el informe, con la direccion de verdad y la pila de verdad.
     *
     * ES CARO A PROPoSITO y no es el defecto de nadie: una reserva de dieciseis
     * bytes cuesta una pagina mas una de guarda, el espacio de direcciones se
     * agota deprisa y cada bloque es una entrada de TLB propia.  Existe para
     * apuntarlo a un programa que ya se sabe roto.
     * \~
     */
    Guard = 4,
};

/**
 * @brief
 * \~english Which edge @c SanLevel::Guard puts the block against.
 * \~spanish Contra que borde pone el bloque @c SanLevel::Guard.
 * \~
 *
 * \~english
 * A block can only be flush against ONE edge of its pages, so the guard catches
 * what happens on that side and nothing on the other.  Which one to watch
 * depends on the bug being hunted, and neither is a better default than the
 * other -- overflow is far more common, so that is what it does unless told
 * otherwise.  Set with `VESTA_ALLOC_SAN_GUARD`.
 *
 * \~spanish
 * Un bloque solo puede quedar pegado a UN borde de sus paginas, asi que la
 * guarda caza lo que pasa por ese lado y nada por el otro.  Cual vigilar depende
 * del fallo que se persiga, y ninguno es mejor defecto que el otro -- el
 * desbordamiento es mucho mas frecuente, asi que es lo que hace salvo que se le
 * diga otra cosa.  Se pone con `VESTA_ALLOC_SAN_GUARD`.
 * \~
 */
enum class SanGuard : unsigned {
    /// \~english Block ends at the page edge: catches writing PAST the end.
    /// \~spanish El bloque acaba en el borde: caza escribir MAS ALLA del final.
    /// \~
    Overflow = 0,
    /// \~english Block starts at the page edge: catches writing BEFORE it.
    /// \~spanish El bloque empieza en el borde: caza escribir ANTES de el.
    /// \~
    Underflow = 1,
};

/**
 * @brief
 * \~english How much of a released block gets poisoned.
 * \~spanish Cuanto de un bloque liberado se envenena.
 * \~
 *
 * \~english
 * Graduated because filling the whole block is O(size) on every release, while
 * the first cache line catches nearly as much for the price of one line.  Set
 * with `VESTA_ALLOC_SAN_POISON`.
 *
 * \~spanish
 * Graduado porque llenar el bloque entero es O(tamano) en cada liberacion,
 * mientras que la primera linea de cache caza casi lo mismo por el precio de
 * una linea.  Se pone con `VESTA_ALLOC_SAN_POISON`.
 * \~
 */
enum class SanPoison : unsigned {
    None = 0,  ///< \~english Do not poison.  \~spanish No envenenar.  \~
    Line = 1,  ///< \~english The first cache line.  \~spanish La primera linea.  \~
    Whole = 2, ///< \~english The whole block.  \~spanish El bloque entero.  \~
};

namespace detail {

/// \~english The level in force.  Read directly; it is set once, at start-up.
/// \~spanish El nivel en vigor.  Se lee directo; se fija una vez, al arrancar.
/// \~
extern SanLevel g_san_level;

} // namespace detail

/**
 * @brief
 * \~english Extra bytes the checker needs behind the caller's request.
 * \~spanish Bytes de mas que el comprobador necesita detras de lo pedido.
 * \~
 *
 * \~english
 * Asked BEFORE the size class is chosen, because the canary has to fit inside
 * the block the allocator ends up handing out -- growing the request is what
 * moves it into a class with room.  With the canary off it returns @p n
 * untouched, so the classes picked are the ordinary ones.
 *
 * @param n what the caller asked for.
 * @return what to allocate instead.
 *
 * @code
 *   const size_t want = util::san_grow(n);   // n, or n + the canary
 * @endcode
 * \~
 *
 * \~spanish
 * Se pregunta ANTES de elegir la clase de tamano, porque el canario tiene que
 * caber dentro del bloque que el asignador acabe entregando -- hacer crecer la
 * peticion es lo que la mueve a una clase con sitio.  Con el canario apagado
 * devuelve @p n tal cual, asi que las clases elegidas son las de siempre.
 *
 * @param n lo que pidio quien llama.
 * @return lo que hay que reservar en su lugar.
 * \~
 */
/**
 * @brief
 * \~english The whole allocation, checker included.  ONE call, not three.
 * \~spanish La reserva entera, comprobador incluido.  UNA llamada, no tres.
 * \~
 *
 * \~english
 * WHY IT IS ONE ENTRY AND NOT THREE.  The first version had `host_alloc` call
 * three separate out-of-line hooks -- grow the size, try the guarded path,
 * record the block -- and each of them asked whether the checker was set up.
 * Measured: an allocation went from 4.9 ns to 12.4 with the checker switched OFF
 * at run time, which is the checker charging for work it was not doing.
 *
 * With one entry the ordinary path calls out once and everything else happens
 * on the other side, where it belongs -- including deciding, on the very first
 * call, whether there is anything to do at all.
 *
 * @param n what the caller asked for.
 * @return the block, or nullptr, exactly like @c host_alloc.
 * \~
 *
 * \~spanish
 * POR QUE UNA ENTRADA Y NO TRES.  La primera version hacia que `host_alloc`
 * llamara a tres ganchos separados fuera de linea -- crecer el tamano, probar
 * el camino con guarda, apuntar el bloque -- y cada uno preguntaba si el
 * comprobador estaba montado.  Medido: una reserva pasaba de 4,9 ns a 12,4 con
 * el comprobador APAGADO en ejecucion, o sea cobrando por un trabajo que no
 * hacia.
 *
 * Con una sola entrada el camino de siempre sale una vez y todo lo demas ocurre
 * al otro lado, que es su sitio -- incluido decidir, en la primera llamada, si
 * hay algo que hacer.
 *
 * @param n lo que pidio quien llama.
 * @return el bloque, o nulo, igual que @c host_alloc.
 * \~
 */
void *san_alloc(size_t n) noexcept;


/**
 * @brief
 * \~english Serves the allocation ITSELF, when the level says pages of its own.
 * \~spanish Sirve la reserva ENTERA, cuando el nivel pide paginas propias.
 * \~
 *
 * \~english
 * The other hooks watch what the allocator did; this one REPLACES it, and it
 * has to: a guarded block lives on pages of its own, outside the allocator's
 * region, so nothing of the ordinary path can hand it out and nothing of the
 * ordinary path may take it back.  Its twin on the way out is @c san_on_free
 * answering false.
 *
 * @param n what the caller asked for.
 * @return the block, or nullptr when this level is not in force -- and then the
 *         ordinary path runs, untouched.
 *
 * @code
 *   void *p = util::san_alloc_guarded(n);
 *   if (p == nullptr) p = util::detail::alloc_body(util::san_grow(n));
 * @endcode
 * \~
 *
 * \~spanish
 * Los demas ganchos miran lo que hizo el asignador; este lo SUSTITUYE, y tiene
 * que hacerlo: un bloque con guarda vive en paginas propias, fuera de la region
 * del asignador, asi que ni el camino de siempre puede entregarlo ni puede
 * recibirlo de vuelta.  Su gemelo a la salida es @c san_on_free contestando
 * false.
 *
 * @param n lo que pidio quien llama.
 * @return el bloque, o nulo cuando este nivel no esta en vigor -- y entonces
 *         corre el camino de siempre, sin tocar.
 * \~
 */
/* \~english `san_grow`, the guarded path and the recording of a block used to
 * be here.  They are INTERNAL now, behind `san_alloc`, and the move is not
 * tidying: each of them read the caller's return address on its own, and
 * reading it from inside another one of them yields an address in the CHECKER
 * instead of in the program.  With one door, the address is read once where it
 * is still true and handed down as an argument.
 *
 * \~spanish `san_grow`, el camino con guarda y el apuntar un bloque estaban aqui.  Ahora
 * son INTERNOS, detras de `san_alloc`, y el cambio no es orden: cada uno leia
 * por su cuenta la direccion de retorno del llamante, y leerla desde dentro de
 * otro de ellos da una direccion del COMPROBADOR en vez de una del programa.
 * Con una sola puerta se lee una vez, donde todavia es cierta, y se pasa como
 * argumento.  \~ */

/**
 * @brief
 * \~english Records a block that has just been handed out.
 * \~spanish Apunta un bloque que se acaba de entregar.
 * \~
 *
 * \~english
 * Deliberately NOT inline: the return address it reads is the instruction after
 * the call, which is the point in the CALLER where the allocation happened --
 * exactly the site the report has to name.  Inlining it would read the frame of
 * whoever inlined it instead.
 *
 * @param p   the block, or nullptr, which does nothing.
 * @param req what the caller asked for, BEFORE @c san_grow.
 * \~
 *
 * \~spanish
 * A proposito NO en linea: la direccion de retorno que lee es la instruccion de
 * despues de la llamada, o sea el punto del LLAMANTE donde ocurrio la reserva
 * -- justo el sitio que el informe tiene que nombrar --.  Ponerla en linea
 * leeria el marco de quien la inlinara.
 *
 * @param p   el bloque, o nulo, que no hace nada.
 * @param req lo que pidio quien llama, ANTES de @c san_grow.
 * \~
 */

/**
 * @brief
 * \~english Checks a block that is about to be released.
 * \~spanish Comprueba un bloque que esta a punto de soltarse.
 * \~
 *
 * \~english
 * @return true when the release must go ahead.  FALSE on a double free, and
 *         that answer matters: releasing it a second time would push the same
 *         block onto the free list twice and hand it to two owners at once, so
 *         the checker would have turned a reported bug into a corrupted heap.
 *
 * @param p the block, or nullptr, which does nothing and answers true.
 * \~
 *
 * \~spanish
 * @return true cuando la liberacion debe seguir adelante.  FALSE en una doble
 *         liberacion, y esa respuesta importa: soltarlo otra vez meteria el
 *         mismo bloque dos veces en la lista de libres y lo entregaria a dos
 *         duenos a la vez, con lo que el comprobador habria convertido un fallo
 *         avisado en un monton corrompido.
 *
 * @param p el bloque, o nulo, que no hace nada y contesta true.
 * \~
 */
bool san_on_free(void *p) noexcept;

/**
 * @brief
 * \~english Whether the checker found anything, for the process exit code.
 * \~spanish Si el comprobador encontro algo, para el codigo de salida.
 * \~
 *
 * \~english
 * A checker that reports a leak and exits zero is the silent success this
 * project spends its time hunting, so the report sets the exit code and this is
 * what a test asks when it wants to assert on it.
 *
 * @return how many verdicts were issued, of any kind.
 * \~
 *
 * \~spanish
 * Un comprobador que avisa de una fuga y sale con cero es el exito silencioso
 * que este proyecto se pasa la vida persiguiendo, asi que el informe fija el
 * codigo de salida y esto es lo que pregunta un test que quiera afirmarlo.
 *
 * @return cuantos veredictos se emitieron, de cualquier clase.
 * \~
 */
uint64_t san_verdicts() noexcept;

/**
 * @brief
 * \~english The longest life measured so far, in allocations of a thread.
 * \~spanish La vida mas larga medida hasta ahora, en reservas de un hilo.
 * \~
 *
 * \~english
 * Here so a test can demand that lives are MEASURED and not merely printed.  A
 * report full of "Instant" looks exactly the same whether the clock works or is
 * stuck at zero -- and it was stuck at zero, reading a field that is only
 * filled in when the report is written.  A test that keeps a block alive across
 * a known number of allocations and then asks for this catches that.
 *
 * @return the largest life recorded, or 0 when nothing has died yet.
 *
 * @code
 *   const uint64_t before = util::san_longest_life();
 *   void *p = util::host_alloc(32);
 *   for (int i = 0; i < 200; ++i) util::host_free(util::host_alloc(16));
 *   util::host_free(p);
 *   // now san_longest_life() >= 200
 * @endcode
 * \~
 *
 * \~spanish
 * Esta aqui para que un test pueda exigir que las vidas se MIDEN y no solo se
 * imprimen.  Un informe lleno de "Instant" tiene el mismo aspecto si el reloj
 * funciona que si esta clavado en cero -- y estaba clavado en cero, leyendo un
 * campo que solo se rellena al escribir el informe.  Un test que mantiene un
 * bloque vivo a lo largo de un numero conocido de reservas y luego pregunta
 * esto lo caza.
 *
 * @return la vida mas larga apuntada, o 0 si no ha muerto nada todavia.
 * \~
 */
uint64_t san_longest_life() noexcept;

/**
 * @brief
 * \~english Every byte handed out so far, released or not.
 * \~spanish Todos los bytes entregados hasta ahora, liberados o no.
 * \~
 *
 * \~english
 * Here for the same reason as @c san_longest_life: so a test can demand that
 * this is MEASURED.  And it answers a question nothing else in this checker
 * could: the shadow is indexed by ADDRESS, so it forgets a block the moment
 * its address is used again, and by the end of a run it knows what LEAKED and
 * not what a site ALLOCATED.  Those come apart hard.  A buffer that doubles
 * twenty times and is released moves gigabytes and leaves nothing behind --
 * invisible to the leak list, and the shape that decides a peak.
 *
 * Counted at BIRTH and not at death, which is the whole point: counting at
 * death would leave out everything still alive at exit.
 *
 * A TOTAL AND NOT THE LARGEST SITE, which it was first: a maximum only moves
 * when something beats it, so a test could not ask "did my two hundred blocks
 * get counted" -- the answer was yes and the number had not budged, because
 * this checker's own symbol resolution had already moved more.  A sum is
 * monotonic, so a difference across a known amount of work is an assertion.
 * Which site moved the most is a question the report answers, with the stack
 * attached, which is where it is useful.
 *
 * @return the total, or 0 when nothing has been allocated yet.
 *
 * @code
 *   const uint64_t before = util::san_moved_bytes();
 *   for (int i = 0; i < 200; ++i) util::host_free(util::host_alloc(16));
 *   // san_moved_bytes() >= before + 3200, even though nothing survived
 * @endcode
 *
 * @code
 *   // and a block past the small-class limit counts too, which is where the
 *   // largest allocations of a real program live
 *   const uint64_t before = util::san_moved_bytes();
 *   util::host_free(util::host_alloc(64u << 20));
 *   // san_moved_bytes() >= before + (64u << 20)
 * @endcode
 *
 * \~spanish
 * Esta aqui por lo mismo que @c san_longest_life: para que un test pueda exigir
 * que esto se MIDE.  Y contesta una pregunta que ninguna otra cosa de este
 * comprobador podia: el sombreado se indexa por DIRECCION, asi que olvida un
 * bloque en cuanto su direccion se vuelve a usar, y al acabar una corrida sabe
 * lo que se FUGO y no lo que un sitio RESERVO.  Y se separan mucho.  Un buffer
 * que se duplica veinte veces y se libera mueve gigabytes y no deja nada
 * detras -- invisible para la lista de fugas, y la forma que decide un pico.
 *
 * Se cuenta al NACER y no al morir, que es de lo que se trata: contar al morir
 * dejaria fuera todo lo que sigue vivo al salir.
 *
 * @return el total mayor, o 0 si no se ha reservado nada todavia.
 * \~
 */
uint64_t san_moved_bytes() noexcept;

/**
 * @brief
 * \~english How much of @p p is usable, when the checker served it itself.
 * \~spanish Cuanto de @p p se puede usar, cuando lo sirvio el comprobador.
 * \~
 *
 * \~english
 * WHY THE ALLOCATOR HAS TO ASK.  At the guard level a block lives on pages of
 * its own, outside both regions and not in the direct table, so the allocator's
 * own question -- `in_region || in_big_region || direct_bytes` -- answers NO
 * for a block it just handed out through this mode.  Everything downstream of
 * that answer then goes wrong: `host_realloc` sends it to `no_foreign_free` and
 * stops the process, `host_usable_size` says zero, and the interposed `realloc`
 * reports out of memory.
 *
 * That is not a corner: it is why the guard level killed fourteen of this
 * library's seventeen tests.  Traced to `pthread_key_create` answering ENOMEM
 * because its `realloc` was refused, then `emutls_init` calling `abort`.
 *
 * ONE FUNCTION FOR TWO QUESTIONS, deliberately: a non-zero answer means "mine",
 * which is what `ours` needs, and the value itself is what `host_usable_size`
 * needs.  Two entry points asking the same table twice is how they drift.
 *
 * \~spanish
 * POR QUE TIENE QUE PREGUNTAR EL ASIGNADOR.  En el nivel de guarda un bloque
 * vive en paginas propias, fuera de las dos regiones y sin estar en la tabla de
 * directos, asi que la pregunta del propio asignador -- `in_region ||
 * in_big_region || direct_bytes` -- contesta NO para un bloque que acaba de
 * entregar por este modo.  Todo lo que cuelga de esa respuesta sale mal:
 * `host_realloc` lo manda a `no_foreign_free` y para el proceso,
 * `host_usable_size` dice cero, y el `realloc` interpuesto avisa de falta de
 * memoria.
 *
 * No es un caso raro: es por lo que el nivel de guarda mataba catorce de los
 * diecisiete tests de esta libreria.  Rastreado hasta `pthread_key_create`
 * contestando ENOMEM porque le rechazaron su `realloc`, y de ahi `emutls_init`
 * llamando a `abort`.
 *
 * UNA FUNCION PARA DOS PREGUNTAS, a proposito: una respuesta distinta de cero
 * significa "es mio", que es lo que necesita `ours`, y el valor en si es lo que
 * necesita `host_usable_size`.  Dos entradas preguntando dos veces a la misma
 * tabla es como se separan.
 * \~
 *
 * @return
 * \~english usable bytes, or 0 when the checker did not serve @p p -- which
 *           includes every level below the guard one.
 * \~spanish bytes utilizables, o 0 si el comprobador no sirvio @p p -- lo que
 *           incluye todos los niveles por debajo del de guarda.
 * \~
 */
/**
 * @brief
 * \~english How many blocks this mode served ITSELF, out of its own pages.
 * \~spanish Cuantos bloques sirvio ESTE modo, de sus propias paginas.
 * \~
 *
 * \~english
 * THE HALF THE ALLOCATOR CANNOT COUNT.  Its own `served` counts blocks it
 * carved out of a region; at the guard level the block comes from pages the
 * checker asked the system for, so the allocator never saw it and rightly does
 * not count it.  That is not a gap to paper over: it is the difference between
 * "what the program asked for" and "what this allocator carved", and it is the
 * number that tells you a run's memory behaviour is not the program's usual
 * one.
 *
 * So the checker counts its own, and the two close an IDENTITY rather than an
 * approximation:
 *
 *     site entries  ==  allocator served  +  san_guarded_blocks()
 *
 * It holds at every level.  Below the guard one this is zero and it degenerates
 * into the equality the allocator already checked, which is what makes it worth
 * asserting instead of the ratio it replaced -- a ratio that was false here by
 * construction and could only be answered by weakening the test.
 *
 * \~spanish
 * LA MITAD QUE EL ASIGNADOR NO PUEDE CONTAR.  Su `served` cuenta bloques que
 * recorto de una region; en el nivel de guarda el bloque sale de paginas que el
 * comprobador le pidio al sistema, asi que el asignador no lo vio y hace bien
 * en no contarlo.  Eso no es un hueco que tapar: es la diferencia entre "lo que
 * pidio el programa" y "lo que recorto este asignador", y es el numero que te
 * dice que el comportamiento de memoria de una corrida no es el habitual del
 * programa.
 *
 * Asi que el comprobador cuenta los suyos, y los dos cierran una IDENTIDAD en
 * vez de una aproximacion:
 *
 *     entradas de sitio  ==  servidas por el asignador  +
 *                            san_guarded_blocks()
 *
 * Se cumple en todos los niveles.  Por debajo del de guarda esto vale cero y
 * degenera en la igualdad que el asignador ya comprobaba, que es lo que la hace
 * digna de afirmarse en lugar de la razon a la que sustituye -- una razon que
 * aqui era falsa por construccion y solo se podia contestar debilitando el
 * test.
 * \~
 *
 * @return
 * \~english how many, or 0 below the guard level.
 * \~spanish cuantos, o 0 por debajo del nivel de guarda.
 * \~
 */
uint64_t san_guarded_blocks() noexcept;

/**
 * @brief
 * \~english The same count as @c san_guarded_blocks, split by PURPOSE.
 * \~spanish La misma cuenta que @c san_guarded_blocks, repartida por PROPOSITO.
 * \~
 *
 * \~english
 * WHY THE SPLIT IS HERE AND NOT ADDED TO THE ALLOCATOR'S, and the data
 * structure decides it: its own comment, where the counting happens, says
 * "counting by tag IS counting: the total comes from summing this table".
 * `by_tag` and `served` are the same number sliced by purpose, so adding a
 * guarded block there would add it to `served` sideways.
 *
 * With this, the identity refines from a total into a per-purpose one, and the
 * total becomes the consequence of the sixteen rather than a rule of its own:
 *
 *     for each tag t:   entries(t)  ==  by_tag(t)  +  san_guarded_by_tag(t)
 *
 * \~spanish
 * POR QUE EL REPARTO ESTA AQUI Y NO SUMADO AL DEL ASIGNADOR, y lo decide la
 * estructura de datos: su propio comentario, donde se cuenta, dice "contar por
 * etiqueta ES contar: el total sale de sumar esta tabla".  `by_tag` y `served`
 * son el mismo numero troceado por proposito, asi que sumar ahi un bloque con
 * guarda seria sumarlo a `served` de lado.
 *
 * Con esto la identidad se refina de un total a un reparto, y el total pasa a
 * ser la consecuencia de las dieciseis en vez de una regla propia:
 *
 *     por cada etiqueta t:   entradas(t)  ==  by_tag(t)  +
 *                            san_guarded_by_tag(t)
 * \~
 *
 * @param tag
 * \~english the purpose, as @c AllocTag::raw gives it.
 * \~spanish el proposito, tal como lo da @c AllocTag::raw.
 * \~
 * @return
 * \~english how many, 0 below the guard level and 0 for a tag out of range.
 * \~spanish cuantos, 0 por debajo del nivel de guarda y 0 si la etiqueta se
 *           sale del rango.
 * \~
 */
uint64_t san_guarded_by_tag(unsigned tag) noexcept;

size_t san_guarded_size(const void *p) noexcept;

/**
 * @brief
 * \~english Resizes @p p when the checker served it; says whether it did.
 * \~spanish Redimensiona @p p si lo sirvio el comprobador; dice si lo hizo.
 * \~
 *
 * \~english
 * A guarded block cannot grow where it lies: its pages end at a guard that has
 * to stay at the end, which is the whole mechanism.  So growing means taking a
 * new one, copying, and releasing the old THROUGH THE CHECKER, so the watch on
 * the old address survives -- release it through the allocator and the address
 * is handed to somebody else, and a use-after-free stops pointing at the code
 * that caused it.
 *
 * @param out where the new pointer goes.  Null means the resize did not happen
 *            and @p p is still valid, which is what `realloc` promises.
 * @return true when this call handled it, false when @p p is not the checker's
 *         and the allocator should carry on.
 *
 * \~spanish
 * Un bloque con guarda no puede crecer donde esta: sus paginas acaban en una
 * guarda que tiene que quedarse al final, que es todo el mecanismo.  Asi que
 * crecer es coger uno nuevo, copiar, y soltar el viejo POR EL COMPROBADOR, para
 * que la vigilancia de la direccion vieja sobreviva -- soltarlo por el
 * asignador entrega esa direccion a otro, y un uso despues de liberar deja de
 * apuntar al codigo que lo causo.
 *
 * @param out donde va el puntero nuevo.  Nulo significa que el cambio de tamano
 *            no ocurrio y @p p sigue siendo valido, que es lo que promete
 *            `realloc`.
 * @return true si esta llamada se ocupo, false si @p p no es del comprobador y
 *         el asignador debe seguir.
 * \~
 */
bool san_realloc(void *p, size_t n, void **out) noexcept;

#else // the checker is not in this build / el comprobador no esta en este build

/* \~english Empty and always inlined: with the macro off, `host_alloc` and
 * `host_free` have to generate the same instructions as if this file did not
 * exist.  That is a promise the build checks, not an intention.
 *
 * \~spanish Vacias y siempre en linea: con la macro apagada, `host_alloc` y
 * `host_free` tienen que generar las mismas instrucciones que si este fichero
 * no existiera.  Es una promesa que el build COMPRUEBA, no una intencion.
 * \~ */

[[gnu::always_inline]] inline void *san_alloc(size_t) noexcept {
    /* \~english Never reached: with the macro off the allocator does not call
     * it.  It is here so a consumer can include this header unconditionally.
     * \~spanish No se alcanza nunca: con la macro apagada el asignador no la
     * llama.  Esta para que quien la use pueda incluir esta cabecera sin
     * condiciones.  \~ */
    return nullptr;
}
[[gnu::always_inline]] inline bool san_on_free(void *) noexcept { return true; }
[[gnu::always_inline]] inline uint64_t san_verdicts() noexcept { return 0; }
[[gnu::always_inline]] inline uint64_t san_longest_life() noexcept { return 0; }
[[gnu::always_inline]] inline uint64_t san_moved_bytes() noexcept { return 0; }
/* \~english Zero, and that is the RIGHT answer rather than a stub: without this
 * mode nobody serves a block but the allocator, so the identity above holds
 * with the second term at zero.  A consumer can assert it unconditionally.
 * \~spanish Cero, y es la respuesta CORRECTA y no un tapon: sin este modo nadie
 * sirve un bloque salvo el asignador, asi que la identidad de arriba se cumple
 * con el segundo termino a cero.  Quien la consuma puede afirmarla sin
 * condiciones.  \~ */
[[gnu::always_inline]] inline uint64_t san_guarded_blocks() noexcept {
    return 0;
}
[[gnu::always_inline]] inline uint64_t san_guarded_by_tag(unsigned) noexcept {
    return 0;
}
[[gnu::always_inline]] inline size_t san_guarded_size(const void *) noexcept {
    return 0;
}
[[gnu::always_inline]] inline bool san_realloc(void *, size_t,
                                               void **) noexcept {
    return false;
}

#endif // VESTA_ALLOC_SANITIZER

} // namespace util

#endif // VESTA_ALLOC_SANITIZER_H

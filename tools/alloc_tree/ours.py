"""Which of these frames is code the program's author wrote.

WHY THIS IS A QUESTION AT ALL.  A tree of four hundred allocation sites is
mostly not the program: it is `std::vector` growing, `std::string` copying, the
C runtime doing its own bookkeeping.  All of it is real memory and none of it
is where the author would look first, so there has to be a way to ask "show me
MY calls" -- and a way that does not lie about the totals when it hides things.

TWO KINDS OF NOT-OURS, and they are told apart differently:

  * ANOTHER MODULE -- `msvcrt.dll`, `libc.so`.  This one is not guessed: the
    export carries a `foreign` column, because only the allocator can know it.
    From out here a system library and a logical module of the project look the
    same in the `module` field.
  * THE STANDARD LIBRARY compiled INTO the program.  There is no fact to carry:
    that code really is inside our binary.  It is recognised by name, which is
    a heuristic, and heuristics get written down rather than hidden -- see
    `is_library_frame`.

AND THE IMPORTANT PART: an allocation made through `std::vector` IS ours.  What
is not ours is the innermost frame, not the allocation.  That is why the folding
mode exists and why it is the recommended one: it walks OUT of the library code
to the first frame the author wrote and hangs the allocation there, instead of
throwing the measurement away.
"""

# Prefixes of names that belong to the C++ standard library or to the
# compiler's own support code.  The export writes names already made readable,
# so `std::vector<...>` is what arrives; the mangled forms are here too because
# a build without a demangler passes them through untouched, and half a rule is
# how something works on one machine and not on the next.
_LIBRARY_PREFIXES = (
    "std::",
    "__gnu_cxx::",
    "__cxxabiv1::",
    "_ZNSt",   # std::  (mangled)
    "_ZSt",    # std::  (mangled, free function)
    "_ZN9__gnu_cxx",
    "operator new",
    "operator delete",
    # El desmanglador de libiberty, que es lo que hay detras de `__cxa_demangle`
    # y de casi todo lo que imprime un nombre de C++.  Llega SIN fichero, asi
    # que por ruta no se le reconoce y caia en "codigo del autor" -- 5,85 MB en
    # la compilacion de 24k lineas, todos del informe poniendo nombres.
    #
    # Como biblioteca y no como instrumento a proposito: si quien desmangla es
    # el programa, el plegado sale hacia fuera y le cuelga la reserva a EL, que
    # es lo correcto.  Marcarlo como aparato de medida se la quitaria.
    "__cxa_demangle",
    "cplus_demangle",
)

# Fragments of a PATH that mean the same thing.  Checked as well as the name
# because an inlined lambda or a helper inside a header carries a name that
# says nothing while its file says everything.
_LIBRARY_PATHS = (
    "/include/c++/",
    "\\include\\c++\\",
    "/bits/",
    "\\bits\\",
    "/ext/",
)


# ---------------------------------------------------------------------------
#  EL INSTRUMENTO, que es una tercera categoria y no una de las dos de arriba
# ---------------------------------------------------------------------------
#
#  Cuando el comprobador recorre la pila, los primeros marcos de TODAS las
#  cadenas son los del propio asignador: el gancho, la cache de hilo, la
#  frontera de `operator new`.  Medido sobre una compilacion de 24k lineas, seis
#  marcos iguales en las 9.542 pilas -- asi que el arbol de dentro hacia fuera
#  no se separa en ramas hasta el sexto nivel, y lo que uno viene a mirar queda
#  debajo de un tronco que no dice nada.
#
#  No son de una libreria ajena ni son del autor: son el APARATO DE MEDIDA.  Y
#  por eso no se borran -- la regla de la casa es que quien mide aparece entre
#  lo medido, y una cadena recortada diria "aqui empezo" donde lo cierto es
#  "aqui empezamos a mirar" --.  Se marcan, se cuentan, y el filtro que ya
#  existe los pliega igual que pliega la biblioteca estandar.
#  Y EL PROPIO INFORME ES LO QUE MAS RESERVA DE TODO.  Medido sobre la
#  compilacion de 24k lineas: `util::report_alloc_sites()` hizo 560.642 reservas
#  y 1,0 GiB leyendo DWARF para poner nombres, mas 179,7 MiB del volcado de
#  estadisticas -- el 63,9 % de los bytes de la poblacion del comprobador.  Mas
#  que el programa medido.
#
#  El comprobador ya se descuenta a si mismo (`g_in_report` en `sanitizer.cpp`),
#  pero ese descuento no cubre el informe del ASIGNADOR, que corre en la lista
#  de salida del runtime de C y para el comprobador es codigo del programa.  Se
#  reconoce aqui, por lo que es -- el lector de DWARF, la tabla de simbolos, el
#  volcado --, y como todo lo demas: se marca, no se borra.
_INSTRUMENT_PATHS = (
    "vesta_alloc/",
    "vesta_alloc\\",
    "util/alloc/",
    "util\\alloc\\",
    "util/alloc_report",
    "util\\alloc_report",
    "util/os/thread_slot",
    "util\\os\\thread_slot",
)

_INSTRUMENT_NAMES = (
    "vesta_alloc_",
    "vesta_crt_",
    "new_measured",
    "new_or_measure",
    "host_alloc",
    "util::host_alloc",
    "util::san_",
    "util::detail::",
    "util::report_alloc_sites",
    "util::dump_alloc_sites",
    "StatsDump",
    "~StatsDump",
    "__wrap_",
)


def is_instrument_frame(frame):
    """True when the frame is the allocator or its checker, not the program.

    Recognised by PATH first, because that is the part that does not depend on
    how a name came out of the demangler.  The names are there for a build with
    no line information, where the path is empty and the name is all there is.
    """
    path = (frame.file or "").replace("\\", "/").lower()
    if any(part.replace("\\", "/") in path for part in _INSTRUMENT_PATHS):
        return True
    return (frame.function or "").startswith(_INSTRUMENT_NAMES)


# ---------------------------------------------------------------------------
#  Y EL ARRANQUE, que hace lo mismo por el otro extremo
# ---------------------------------------------------------------------------
#
#  Al final de toda cadena estan los marcos con los que el sistema entra en un
#  hilo o en el programa: siete en esa misma compilacion.  De fuera hacia dentro
#  son el tronco comun de todo, por la misma razon y con el mismo efecto.
#
#  LA SALIDA CUENTA IGUAL QUE LA ENTRADA, y no es una ampliacion gratuita: el
#  informe del asignador corre DESDE la lista de salida del runtime de C, asi
#  que su cadena acaba en `run_exit_list` -- y mientras eso pasara por codigo
#  del autor, el plegado se paraba ahi y el gigabyte del informe se quedaba en
#  el arbol.  Son los marcos con los que el sistema entra y SALE.
#
#  Van APARTE del instrumento aunque el arbol los trate igual: uno es codigo
#  nuestro que existe porque estamos midiendo y el otro es del sistema y estaria
#  ahi de todas formas.  Contarlos juntos haria imposible decir cual de las dos
#  cosas esta tapando la vista.
_STARTUP_NAMES = (
    # La entrada
    "BaseThreadInitThunk",
    "RtlUserThreadStart",
    "_beginthreadex",
    "_endthreadex",
    "mainCRTStartup",
    "__tmainCRTStartup",
    "WinMainCRTStartup",
    "pre_cpp_init",
    "__do_global_ctors",
    "pthread_create_wrapper",
    "execute_native_thread_routine",
    "__libc_start_main",
    "_start",
    "start_thread",
    "clone",
    # Y la salida, que es por donde corre lo que se registro con `atexit`
    "run_exit_list",
    "_initterm",
    "_initterm_e",
    "__do_global_dtors",
    "__run_exit_handlers",
    "doexit",
)


def is_startup_frame(frame):
    """True when the frame is how the system entered or left this thread.

    Both ends on purpose: what runs from the exit list is not the program
    doing its work either, and the allocator's own report runs from exactly
    there.
    """
    return (frame.function or "") in _STARTUP_NAMES


def is_library_frame(frame):
    """True when this frame is standard-library or compiler support code.

    A HEURISTIC, and it is one on purpose: this code is compiled INTO the
    program, so unlike a foreign module there is no fact to carry -- there is
    only what it is called and which header it came from.  It is kept in one
    place, with the lists visible, so that when it is wrong it is wrong
    somewhere findable.
    """
    name = frame.function or ""
    if name.startswith(_LIBRARY_PREFIXES):
        return True
    path = (frame.file or "").lower()
    if any(part in path for part in _LIBRARY_PATHS):
        return True
    # EL DESMANGLADOR DE LIBIBERTY, por familia y no por nombre.  Sus funciones
    # se llaman todas `d_algo` -- `d_demangle_callback`, `d_growable_string_*`,
    # `d_append_char`, `d_print_comp` -- y vienen SIN fichero, asi que ni la
    # ruta las coloca ni hay forma de listarlas sin ir una a una segun aparecen.
    # Reservan de verdad: 5,84 MB en la compilacion de 24k lineas, y todo para
    # poner nombres legibles en el informe.
    #
    # El "sin fichero" es la mitad que hace la regla estrecha.  Sigue siendo una
    # HEURISTICA, y falla en el caso que se puede decir: una funcion del usuario
    # llamada `d_loquesea` en una construccion despojada.  Queda escrita aqui
    # para que ese fallo se encuentre.
    return not path and name.startswith("d_")


# ---------------------------------------------------------------------------
#  DONDE SE APLICA ESTO, Y POR QUE NO AQUI
# ---------------------------------------------------------------------------
#
#  Esta clasificacion viaja a la pagina, un bit por marco, y es la PAGINA la
#  que filtra.  Filtrar es una forma de MIRAR, no de exportar: quien lee el
#  informe cambia de idea tres veces mientras lo mira, y si el filtro fuera una
#  bandera al generar habria que rehacerlo cada vez -- con el riesgo de acabar
#  comparando dos informes que no son el mismo.  El HTML lleva SIEMPRE todo lo
#  medido y decide al mirar.
#
#  Los dos modos que ofrece la pagina, y que son dos lecturas honestas de la
#  misma pregunta:
#
#    PLEGAR   una reserva hecha A TRAVES de la biblioteca sigue siendo nuestra:
#             se sale hacia fuera hasta el primer marco del autor y se cuelga
#             ahi.  No se pierde ni una; cambia de donde cuelga.
#    OCULTAR  fuera si no empieza en codigo nuestro.  Mas romo, y ese SI pierde
#             reservas -- por eso la pagina dice siempre cuantas se dejo fuera.
#
#  Un sitio de otro modulo no tiene ningun marco nuestro, asi que los dos modos
#  lo dejan fuera; y eso no lo decide la heuristica de aqui, sino la columna
#  `foreign`, que es un hecho medido.


# ---------------------------------------------------------------------------
#  Y DE QUE MARCO CUELGA UNA RESERVA
# ---------------------------------------------------------------------------
def owning_frame(chain):
    """El marco al que se le cuelga la reserva: el primero, de dentro hacia
    fuera, que escribio el autor.

    ES EL MODO "PLEGAR" de arriba, escrito una vez.  Lo usan la pagina
    (`groupNode` en `tree.js`, `timeOwnFrame` en `time.js`), el arbol de la
    terminal (`tree.group_frame`) y el modo consulta (`query.chain_fields`), y
    tienen que usar el mismo o el mismo sitio sale con dos nombres segun por
    donde se mire.

    NO EL DE MAS AFUERA, y esto costo una medida para verse: con el, subir el
    recorrido de pila a ocho marcos mandaba 7.536 de 8.611 sitios a
    `KERNEL32.DLL`, `ntdll.dll` o el arranque del CRT -- cuanto mas lejos se
    camina, mas se parece ese marco a como entro el sistema en el hilo, que es
    el mismo para todo el programa y no distingue nada.  Agrupar por modulo
    contestaba y no decia nada.

    Sin ningun marco del autor se devuelve el de mas adentro: una pila que de
    verdad es toda biblioteca es de esa biblioteca, no del arranque del hilo.
    """
    for frame in chain:
        if (not is_instrument_frame(frame)
                and not is_startup_frame(frame)
                and not is_library_frame(frame)):
            return frame
    return chain[0] if chain else None

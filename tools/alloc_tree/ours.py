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
    return any(part in path for part in _LIBRARY_PATHS)


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

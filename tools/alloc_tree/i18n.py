# -*- coding: utf-8 -*-
#
# VestaVM -- Distributed Virtual Machine
#
# Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
# License: MIT (see LICENSE).  Part of the VestaVM family.
"""Everything the reader sees, in one place, in every language.

WHY A CATALOGUE AND NOT SENTENCES WHERE THEY ARE PRINTED.  Because a string
written at the point of use exists in exactly one language for ever: adding a
second means finding all of them first, and nothing tells you when you missed
one -- the page simply comes out half translated.  Here, a missing key is
visible as a key.

The catalogue serves BOTH views.  The terminal formats with `t()`; the page
carries the whole table and switches without reloading, because the reader who
wants the other language is not going to run the tool again.

WHAT IS NOT IN HERE: the measurement.  Function names, file paths, modules and
purposes come out of the binary and are not text to translate -- turning
`long/growing` into `largo/creciente` would make the page and the export
disagree about what a thing is called.
"""

# The name of each language IN that language, which is how a person finds
# their own in a list.
LANGS = (("en", "English"), ("es", "Espanol"))

STRINGS = {
    "en": {
        "tab.tree": "Allocation tree",
        "tab.raw": "The export, raw",
        "tool.grouping": "Grouping:",
        "tool.lang": "Language:",
        "group.stack": "Call stack",
        "group.module": "Module -> call stack",
        "group.file": "Source file -> call stack",
        "group.purpose": "Purpose -> call stack",
        "tool.scope": "Show:",
        "scope.all": "everything measured",
        "scope.fold": "only my calls (through the library)",
        "scope.hide": "only my calls (strictly)",
        "scope.extern": "only code I did not write",
        "tool.langfilter": "Language:",
        "lang.all": "C and C++",
        "lang.c": "C only",
        "lang.cpp": "C++ only",
        "lang.unknown": "language unknown",
        "lang.no_info": "%(allocs)s of those have no language to go by: with "
                        "no debug information there is no file, and a bare "
                        "name does not say which language wrote it. Build with "
                        "-g and the .c and .cpp files answer it outright",
        "scope.left_out": "%(allocs)s allocations (%(bytes)s) left out: another "
                          "module, or library code with nobody of yours behind "
                          "it",
        "dir.td": "from the binary inwards",
        "dir.bu": "from the allocation outwards",
        "ph.filter": "filter by function, file, module or purpose",
        "chk.bytes": "weigh by bytes",
        "btn.expand": "expand hot path",
        "btn.collapse": "collapse all",
        "col.name": "Function stack",
        "col.total": "Allocations",
        "col.pct": "%",
        "col.self": "Self",
        "col.bytes": "Bytes",
        "col.selfbytes": "Bytes: self",
        "col.large": "Large",
        "col.classes": "Classes",
        "col.purpose": "Purpose",
        "col.sites": "Sites",
        "col.module": "Module",
        "col.file": "Source file",
        "col.addr": "Offset",
        "tip.large": "allocations too big for the slab",
        "tip.classes": "distinct size classes touched by this branch",
        "tip.upper": "upper bound: inherited from an evicted entry",
        "stat": "{rows} rows shown of {nodes} in the tree - "
                "{allocs} allocations - {bytes} requested",
        "empty": "nothing matches \"{needle}\"",
        "det.what": "what it was for",
        "det.sites": "the {n} sites in this branch, as measured",
        "det.allocations": "{n} allocations",
        "det.upper": "(+{n} upper bound)",
        "det.requested": "{n} requested",
        "det.endhere": "{n} end here ({bytes})",
        "det.nsites": "{n} sites",
        "det.average": "{n} average",
        "det.fn": "function",
        "det.inner": "allocates in",
        "det.source": "source",
        "det.module": "module",
        "raw.note": "The five files exactly as write_alloc_csv wrote them -- "
                    "every row, every column, including columns this tool has "
                    "no code for. What a branch of the tree was made of is "
                    "answered up there, next to the branch; this is here so "
                    "nothing is only available folded.",
        "raw.filter": "filter {what}",
        "raw.rows": "{n} rows",
        "raw.shown": "{shown} of {total} rows",
        "sub": "{sites} sites - {allocs} small allocations - {bytes} committed",
        "sub.unresolved": "{n} unresolved",
        "file.sites": "one row per site: what it allocated, its purpose and "
                      "its address",
        "file.frames": "one row per (site, depth): the inlining chain, "
                       "innermost first",
        "file.sizes": "the histogram of requested sizes, for the whole process",
        "file.site_sizes": "the same histogram PER SITE, sparse: only the "
                           "buckets a site actually used",
        "det.sizes": "how big they were",
        "det.nosizes": "this export has no per-site split of sizes: it was "
                       "written before that file existed",
        "file.tags": "how much each declared purpose accounts for",
        "file.summary": "the totals, and what could not be resolved",
        "file.check_sites": "one row per walked STACK: what it moved, what it "
                            "left behind, and what its blocks turned out to be",
        "file.check_frames": "one row per (stack, frame, depth): `frame` is "
                             "the call frame the walk found, `depth` the "
                             "inline level inside it -- two different axes",
        "file.check_summary": "what the checker could not cover, and why",
        "head.totals": "{allocs} allocations across {sites} sites, "
                       "{bytes} committed",
        "head.tree": "tree (allocs, % of total, bytes, self, purpose, "
                     "module, function)",
        "warn.nosymbols": "no symbol resolver was installed: the tree carries "
                          "addresses instead of names",
        "warn.unresolved": "{n} sites could not be resolved",
        "warn.full": "the snapshot filled up ({n}): there may be more sites",
        "warn.evicted": "{n} allocations evicted a weaker entry, so those "
                        "counts are lower bounds",
        "warn.deep": "some chain reached the frame limit ({n}) and may be cut",
        "warn.check.depotfull": "the checker's stack depot filled up ({cap}): "
                                "{n} stacks were not recorded",
        "warn.check.nosite": "{n} blocks ({b} bytes) had no stack to charge "
                             "them to",
        "warn.check.outside": "{n} blocks were outside the shadow and could "
                              "not be tracked",
        "warn.check.nowalk": "{n} of {total} stacks are ONE frame: the walk "
                             "found no frame pointer to follow. Build with "
                             "-fno-omit-frame-pointer and they become real "
                             "stacks",
        "chk.title": "The checker: real stacks, measured purpose",
        "chk.sub": "A different population from the table above, not more "
                   "rows of it. Here a site is a walked STACK, the purpose is "
                   "what the blocks turned out to BE -- not what anybody "
                   "declared -- and the blocks over the small-class limit are "
                   "weighed but not inspected",
        "chk.col.weighed": "Only weighed",
        "chk.col.alive": "Alive at exit",
        "chk.col.life": "Life (avg/max)",
        "chk.col.sizes": "Sizes",
        "chk.col.walked": "Walked",
        "chk.none": "this run had no checker export "
                    "(check_sites.csv is missing)",
        "tool.dataset": "Measurement:",
        "data.alloc": "allocator: every block, one return address",
        "data.check": "checker: walked stacks, measured purpose",
    },
    "es": {
        "tab.tree": "Arbol de reservas",
        "tab.raw": "El volcado, crudo",
        "tool.grouping": "Agrupar por:",
        "tool.lang": "Idioma:",
        "group.stack": "Pila de llamadas",
        "group.module": "Modulo -> pila de llamadas",
        "group.file": "Fichero -> pila de llamadas",
        "group.purpose": "Proposito -> pila de llamadas",
        "tool.scope": "Mostrar:",
        "scope.all": "todo lo medido",
        "scope.fold": "solo mis llamadas (a traves de la biblioteca)",
        "scope.hide": "solo mis llamadas (en sentido estricto)",
        "scope.extern": "solo codigo que yo no escribi",
        "tool.langfilter": "Lenguaje:",
        "lang.all": "C y C++",
        "lang.c": "solo C",
        "lang.cpp": "solo C++",
        "lang.unknown": "lenguaje sin determinar",
        "lang.no_info": "%(allocs)s de esas no tienen con que saberlo: sin "
                        "informacion de depuracion no hay fichero, y un nombre "
                        "a secas no dice en que lenguaje se escribio. Compila "
                        "con -g y los .c y .cpp lo contestan solos",
        "scope.left_out": "%(allocs)s reservas (%(bytes)s) fuera: de otro "
                          "modulo, o codigo de biblioteca sin nadie tuyo "
                          "detras",
        "dir.td": "de fuera hacia dentro",
        "dir.bu": "de dentro hacia fuera",
        "ph.filter": "filtrar por funcion, fichero, modulo o proposito",
        "chk.bytes": "pesar por bytes",
        "btn.expand": "desplegar el camino caliente",
        "btn.collapse": "plegarlo todo",
        "col.name": "Pila de funciones",
        "col.total": "Reservas",
        "col.pct": "%",
        "col.self": "Propias",
        "col.bytes": "Bytes",
        "col.selfbytes": "Bytes propios",
        "col.large": "Grandes",
        "col.classes": "Clases",
        "col.purpose": "Proposito",
        "col.sites": "Sitios",
        "col.module": "Modulo",
        "col.file": "Fichero",
        "col.addr": "Desplazamiento",
        "tip.large": "reservas demasiado grandes para el slab",
        "tip.classes": "clases de tamano distintas que toca esta rama",
        "tip.upper": "cota superior: heredado de una entrada desalojada",
        "stat": "{rows} filas de {nodes} en el arbol - "
                "{allocs} reservas - {bytes} pedidos",
        "empty": "nada coincide con \"{needle}\"",
        "det.what": "para que era",
        "det.sites": "los {n} sitios de esta rama, tal como se midieron",
        "det.allocations": "{n} reservas",
        "det.upper": "(+{n} de cota superior)",
        "det.requested": "{n} pedidos",
        "det.endhere": "{n} acaban aqui ({bytes})",
        "det.nsites": "{n} sitios",
        "det.average": "{n} de media",
        "det.fn": "funcion",
        "det.inner": "reserva en",
        "det.source": "fuente",
        "det.module": "modulo",
        "raw.note": "Los cinco ficheros tal como los escribio write_alloc_csv "
                    "-- todas las filas, todas las columnas, incluidas las "
                    "que esta herramienta no sabe interpretar. De que esta "
                    "hecha una rama del arbol se contesta ahi arriba, al lado "
                    "de la rama; esto esta aqui para que nada exista solo "
                    "plegado.",
        "raw.filter": "filtrar {what}",
        "raw.rows": "{n} filas",
        "raw.shown": "{shown} de {total} filas",
        "sub": "{sites} sitios - {allocs} reservas pequenas - "
               "{bytes} comprometidos",
        "sub.unresolved": "{n} sin resolver",
        "file.sites": "una fila por sitio: lo que reservo, su proposito y su "
                      "direccion",
        "file.frames": "una fila por (sitio, profundidad): la cadena de "
                       "inline, de dentro hacia fuera",
        "file.sizes": "el reparto de tamanos pedidos, de todo el proceso",
        "file.site_sizes": "el mismo reparto POR SITIO, disperso: solo las "
                           "casillas que cada sitio uso de verdad",
        "det.sizes": "de que tamano eran",
        "det.nosizes": "este volcado no trae el reparto por sitio: se escribio "
                       "antes de que ese fichero existiera",
        "file.tags": "cuanto se lleva cada proposito declarado",
        "file.summary": "los totales, y lo que no se pudo resolver",
        "file.check_sites": "una fila por PILA recorrida: cuanto movio, cuanto "
                            "dejo detras, y que resultaron ser sus bloques",
        "file.check_frames": "una fila por (pila, marco, profundidad): `frame` "
                             "es el marco de llamada que encontro el "
                             "recorrido, `depth` el nivel de inline dentro de "
                             "el -- dos ejes distintos",
        "file.check_summary": "lo que el comprobador no pudo cubrir, y por que",
        "head.totals": "{allocs} reservas en {sites} sitios, "
                       "{bytes} comprometidos",
        "head.tree": "arbol (reservas, % del total, bytes, propias, "
                     "proposito, modulo, funcion)",
        "warn.nosymbols": "no habia resolutor de simbolos: el arbol lleva "
                          "direcciones en vez de nombres",
        "warn.unresolved": "{n} sitios no se pudieron resolver",
        "warn.full": "la foto se lleno ({n}): puede haber mas sitios",
        "warn.evicted": "{n} reservas desalojaron a una entrada mas floja, "
                        "asi que esas cuentas son cotas inferiores",
        "warn.deep": "alguna cadena llego al tope de marcos ({n}) y puede "
                     "estar cortada",
        "warn.check.depotfull": "el deposito de pilas del comprobador se lleno "
                                "({cap}): {n} pilas no se registraron",
        "warn.check.nosite": "{n} bloques ({b} bytes) no tenian pila a la que "
                             "cargarlos",
        "warn.check.outside": "{n} bloques quedaron fuera de la sombra y no se "
                              "pudieron seguir",
        "warn.check.nowalk": "{n} de {total} pilas son de UN marco: el "
                             "recorrido no encontro puntero de marco que "
                             "seguir. Compilando con -fno-omit-frame-pointer "
                             "pasan a ser pilas de verdad",
        "chk.title": "El comprobador: pilas reales, proposito medido",
        "chk.sub": "Es otra poblacion, no mas filas de la tabla de arriba. "
                   "Aqui un sitio es una PILA recorrida, el proposito es lo "
                   "que los bloques resultaron SER -- no lo que alguien "
                   "declaro -- y los bloques por encima del limite de clase "
                   "pequena se pesan pero no se inspeccionan",
        "chk.col.weighed": "Solo pesado",
        "chk.col.alive": "Vivo al salir",
        "chk.col.life": "Vida (media/max)",
        "chk.col.sizes": "Tamanos",
        "chk.col.walked": "Recorrida",
        "chk.none": "esta corrida no trae exportacion del comprobador "
                    "(falta check_sites.csv)",
        "tool.dataset": "Medida:",
        "data.alloc": "asignador: todo bloque, una direccion de retorno",
        "data.check": "comprobador: pilas recorridas, proposito medido",
    },
}


def t(lang, key, **kw):
    """The string for @p key, with its parameters filled in.

    A key with no entry comes back AS THE KEY, in brackets.  It is ugly on
    purpose: a missing translation that falls back silently to English reads
    like a finished page, and nobody ever fixes it.
    """
    table = STRINGS.get(lang) or STRINGS["en"]
    text = table.get(key)
    if text is None:
        text = STRINGS["en"].get(key)
    if text is None:
        return "[%s]" % key
    try:
        return text.format(**kw) if kw else text
    except (KeyError, IndexError):
        # A parameter the catalogue asks for and the caller did not pass.
        # Said, not hidden: the sentence would come out mangled anyway.
        return "[%s: bad parameters]" % key

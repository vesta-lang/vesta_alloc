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
        "stat.instrument": "%(allocs)s allocations (%(bytes)s) were the REPORT "
                           "itself reading symbols -- not the program, and not "
                           "in this tree",
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
        # LAS DOS CIFRAS AL FILTRAR.  Una tabla que encoge en silencio se lee
        # como una exportacion mas corta.
        "raw.some": "{n} of {all} rows",
        "raw.shown": "{shown} of {total} rows",
        "sub": "{sites} sites - {allocs} small allocations - {bytes} committed",
        # THE HEADLINE SAYS WHICH NUMBER IT IS.  "committed" alone was read as
        # "what the program needed" and it was the running total, which counts a
        # reused range again: 3,328 MiB announced for a process that never held
        # more than 2,374.  Two keys, because they are two different questions.
        "sub.peak": "{sites} sites - {allocs} small allocations - "
                    "{bytes} at its peak",
        "sub.overrun": "{sites} sites - {allocs} small allocations - "
                       "{bytes} committed over the run (cumulative)",
        "sub.unresolved": "{n} unresolved",
        "file.sites": "one row per site: what it allocated, its purpose and "
                      "its address",
        "file.frames": "one row per (site, depth): the inlining chain, "
                       "innermost first",
        "file.sizes": "the histogram of requested sizes, for the whole process",
        "file.site_sizes": "the same histogram PER SITE, sparse: only the "
                           "buckets a site actually used, counted and weighed",
        "file.check_sizes": "what each size KEPT: handed out, given back, and "
                            "therefore still standing. The two above are about "
                            "the whole run, and a bucket that moves four "
                            "gigabytes and gives them all back costs nothing",
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
        "file.check_pairs": "one row per (site that handed out, site that gave "
                            "back): a site whose blocks all come back through "
                            "ONE place has an owner; through several, it is "
                            "shared",
        "file.check_summary": "what the checker could not cover, and why",
        "file.check_epochs": "one row per CUT of the run: what was live at that "
                             "moment, what the allocator had taken from the "
                             "system, and what was handed out and given back "
                             "since the cut before",
        "file.check_epoch_sites": "one row per (cut, site): what that site was "
                                  "holding at that moment -- the curve of each "
                                  "site, and who was live at the same time as "
                                  "whom",
        "file.check_peak": "the cut where the most was live, site by site: the "
                           "one instant that decides how much memory a run "
                           "needs",
        # \~english THE NAMES OF ANOTHER PROGRAM'S PHASES, translated here.
        # What `san_mark` stores is a KEY, never a sentence: the checker is a
        # library and has no business carrying somebody else's words in
        # somebody else's language.  This is where translations already live,
        # so this is where the words go.  A key with no entry is shown as it
        # came, which is a label nobody wrote rather than a blank.
        "mark.vx.phase.resolve": "resolving the module graph",
        "mark.vx.phase.modules": "compiling the modules",
        "mark.vx.phase.optimize": "optimizing the IR",
        "mark.vx.phase.emit": "emitting the .vel",
        "mark.vx.phase.link": "assembling and linking",
        "tab.sizes": "by size",
        "sz.note": "The same histogram read four ways. Counting hides what "
                   "this is for: a bucket holds a RANGE of sizes, so the last "
                   "one -- everything over 16 MiB -- comes out as a handful of "
                   "allocations next to tens of millions of tiny ones, and in "
                   "bytes it can be the one that decides the peak.",
        "sz.draw": "draw",
        "sz.allocs": "allocations",
        "sz.bytes": "bytes asked for",
        "sz.live": "still live",
        "sz.freed": "given back",
        "sz.bucket": "up to",
        "sz.frees": "releases",
        "sz.livemax": "most live at once",
        "sz.unknown": "not known",
        "sz.noliveaxis": "What is still live is not in this export: a block "
                         "too big for the shadow is only recognised on its way "
                         "back by the table the axis keeps, so without "
                         "VESTA_ALLOC_SAN_EPOCH everything above the small "
                         "classes is born and never dies. The columns are "
                         "empty and not zero on purpose -- a zero there would "
                         "read as \"this size keeps nothing\".",
        "tab.time": "over time",
        "time.note": "How much memory was live at each moment, and whose it "
                     "was. The tree answers who allocates; this answers WHEN, "
                     "which is the question that decides how much memory a run "
                     "needs -- two sites that were never live at the same "
                     "instant cost what the bigger one costs, and in a tree "
                     "they add up.",
        "time.what": "draw",
        "time.both": "live and committed",
        "time.onlylive": "live only",
        "time.onlycomm": "committed only",
        "time.top": "sites",
        "time.stack": "stack them",
        # SEIS FORMAS DE MIRAR LO MISMO.  Cada una contesta una pregunta que las
        # otras no: ver la descripcion de cada opcion.
        "time.draw": "draw",
        "time.d.stack": "stacked",
        "time.d.lines": "lines",
        "time.d.log": "log axis",
        "time.d.pct": "100% stacked",
        "time.d.grid": "one chart each",
        "time.d.bubbles": "bubbles",
        "time.nolog": "a value of zero has no place on a log axis: the floor is {v}, and a band that falls under it is drawn on it",
        "time.pctaxis": "share of what was live",
        "time.bubbles.at": "what it was made of at cut {i}{mark} -- {n} bands, {bytes} live",
        "time.bubbles.peaks": "{n} bands, each at ITS OWN peak -- they did not happen at the same time, so they do not add up",
        "time.bubbles.rest": "the rest",
        "time.nopick": "nothing is picked: choose a band below",
        "time.share": "share",
        "time.hideown": "leave out what the report spent on itself",
        "time.filter": "filter sites",
        "time.live": "live",
        "time.committed": "committed",
        # LA REGION Y LA HOLGURA.  "region" a secas no dice de que se habla, y
        # la resta sin nombre se lee como un tercer total.
        "time.region": "the allocator's ranges",
        "time.slack": "held and not handed out",
        # Y LAS DOS MITADES DE ESE HUECO, que tienen curas
        # distintas: una vuelve al sistema y la otra no.
        "time.freespans": "...in free spans (could go back)",
        "time.emptychunks": "...in chunks with every block dead (could too)",
        "time.inclasses": "...in chunks with a few alive, scattered (could not)",
        "time.churn": "moved since the cut before",
        "time.cut": "cut {i} · {a} allocations",
        "time.xaxis": "{n} allocations",
        "time.back": "bring back {n}",
        "time.nomark": "no phase",
        "time.peakhead": "at the peak: {bytes} live, in {mark}",
        "time.col.bytes": "live",
        "time.col.share": "share",
        "time.col.growth": "shape",
        "time.group": "group by",
        "time.g.site": "call stack",
        "time.g.function": "function",
        "time.g.file": "source file",
        "time.g.module": "module",
        "time.g.size": "size",
        "time.g.shape": "shape, as measured",
        "time.g.life": "how long they lived",
        # WHAT TELLS THEM APART GOES FIRST.  These used to read "lived under
        # {n} allocations", so every band began with the same eighteen
        # characters and differed only at the end -- which is exactly what a
        # legend chip and a tooltip cell cut off.  Eight bands came out looking
        # identical, and the panel said nothing.  The axis is named once, in
        # the "group by" selector; a band only has to name its VALUE.
        "time.life.upto": "< {n} allocations",
        "time.life.none": "never died",
        "time.z.in": "+",
        "time.z.out": "-",
        "time.z.all": "all of it",
        "time.help": "drag to move · wheel or the buttons to zoom · "
                     "shift-drag picks a stretch · double-click goes back · "
                     "the box itself can be made taller",
        "time.all": "all of them",
        "time.none": "none",
        "time.hidden": "{n} more not listed by the filter",
        "time.more": "{n} more",
        "time.unzoom": "the whole run",
        "time.scale": "scale",
        "time.s.all": "the whole run",
        "time.s.picked": "what is picked",
        "time.stacks": "{n} call stacks",
        # DE QUE FICHEROS ES UNA BANDA.  Una banda de forma o de vida dice que
        # CLASE de memoria es y no a que codigo ir; esto es lo segundo.
        "time.byfile": "where it comes from -- {n} file(s), measured cut by cut",
        "time.nofile": "(no file)",
        "time.mostly": "mostly {file}",
        "time.nostacks": "a size bucket is not a place: this curve is counted cut by cut, not folded out of the stacks, so there is no call chain to open",
        "tool.theme": "theme",
        "theme.auto": "as the system",
        "theme.light": "light",
        "theme.dark": "dark",
        "head.totals": "{allocs} allocations across {sites} sites, "
                       "{bytes} at its peak",
        "head.totals.run": "{allocs} allocations across {sites} sites, "
                           "{bytes} committed over the run (cumulative)",
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
        "warn.check.cut": "{n} of {total} stacks are CUT: the walk had more to "
                          "follow and nowhere to put it, so they hang from "
                          "where they were cut and not from who called",
        "warn.check.poolfull": "{n} frames found no room in the store, so the "
                               "stacks that wanted them are shorter than what "
                               "was walked",
        "warn.check.indexfull": "{n} granules found no room in the index: an "
                                "address INSIDE those blocks cannot be placed",
        "warn.check.pairsfull": "{n} releases found no room in the pairs table, "
                                "so who owns what is what FIT, not what "
                                "happened",
        "warn.check.noguard": "{n} allocations got no guarded block -- the "
                              "table was full -- and were served the ordinary "
                              "way, so a mistake in them is not caught at the "
                              "instant",
        "warn.check.noshadow": "{n} chunks the system would not give shadow "
                               "memory for: their blocks have no leak verdict",
        "gap.level.canary": "no overflow is caught here: that needs the canary "
                            "level (VESTA_ALLOC_SAN=2)",
        "gap.level.poison": "no write-after-free is caught here: that needs "
                            "the poison level (VESTA_ALLOC_SAN=3)",
        "gap.level.guard": "nothing is caught AT THE INSTANT here: that needs "
                           "the guard level (VESTA_ALLOC_SAN=4)",
        "gap.level.noshadow": "no leaks and no lives here: at the guard level "
                              "blocks come out of pages of their own and never "
                              "reach the shadow that would have watched them",
        "chk.verdicts": "WHAT WAS WRONG: {n} findings, each with the stacks "
                        "that made it. A proven one is not an opinion",
        "warns.summary": "{n} things could not be covered and {gaps} the level "
                         "does not look at -- click to read them",
        "chk.verdict.at": "allocated at",
        "cert.proven": "PROVEN",
        "cert.suspected": "SUSPECTED",
        "file.check_verdicts": "one row per finding: what was wrong, at which "
                               "address, and the stacks -- allocated, "
                               "released, released again",
        "chk.owners": "who gives back what, from {pairs} pairs: {one} sites "
                      "have ONE owner and could go to an arena of their own, "
                      "{many} are shared and belong on the common path, and "
                      "{none} had nothing released, which is neither",
        "col.owners": "sites that gave its blocks back: 1 means it has an "
                      "owner, more than 1 that it is shared",
        "gap.partial.allocator": "this export has the checker's half only: the "
                                 "allocator's own tables were not written, so "
                                 "what is below is every walked stack and NOT "
                                 "every allocation",
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
        "stat.instrument": "%(allocs)s reservas (%(bytes)s) fueron el propio "
                           "INFORME leyendo simbolos -- no son del programa, y "
                           "no estan en este arbol",
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
        "raw.some": "{n} de {all} filas",
        "raw.shown": "{shown} de {total} filas",
        "sub": "{sites} sitios - {allocs} reservas pequenas - "
               "{bytes} comprometidos",
        # EL TITULAR DICE QUE CIFRA ES.  "comprometidos" a secas se leia como
        # "lo que el programa necesito" y era el acumulado, que cuenta un rango
        # reusado otra vez: 3.328 MiB anunciados para un proceso que nunca tuvo
        # mas de 2.374.  Dos claves, porque son dos preguntas distintas.
        "sub.peak": "{sites} sitios - {allocs} reservas pequenas - "
                    "{bytes} en su maximo",
        "sub.overrun": "{sites} sitios - {allocs} reservas pequenas - "
                       "{bytes} comprometidos en toda la corrida (acumulado)",
        "sub.unresolved": "{n} sin resolver",
        "file.sites": "una fila por sitio: lo que reservo, su proposito y su "
                      "direccion",
        "file.frames": "una fila por (sitio, profundidad): la cadena de "
                       "inline, de dentro hacia fuera",
        "file.sizes": "el reparto de tamanos pedidos, de todo el proceso",
        "file.site_sizes": "el mismo reparto POR SITIO, disperso: solo las "
                           "casillas que cada sitio uso de verdad, contadas y "
                           "pesadas",
        "file.check_sizes": "lo que cada tamano SE QUEDO: entregado, devuelto "
                            "y por tanto lo que sigue en pie. Los dos de "
                            "arriba hablan de toda la corrida, y una casilla "
                            "que mueve cuatro gigabytes y los devuelve todos "
                            "no cuesta nada",
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
        "file.check_pairs": "una fila por (sitio que entrego, sitio que "
                            "devolvio): un sitio cuyos bloques vuelven todos "
                            "por UN sitio tiene dueño; por varios, es "
                            "compartido",
        "file.check_summary": "lo que el comprobador no pudo cubrir, y por que",
        "file.check_epochs": "una fila por CORTE de la corrida: lo que habia "
                             "vivo en ese momento, lo que el asignador llevaba "
                             "pedido al sistema, y lo entregado y devuelto "
                             "desde el corte anterior",
        "file.check_epoch_sites": "una fila por (corte, sitio): lo que ese "
                                  "sitio tenia en ese momento -- la curva de "
                                  "cada sitio, y quien estaba vivo a la vez "
                                  "que quien",
        "file.check_peak": "el corte con mas memoria viva, sitio a sitio: el "
                           "unico instante que decide cuanta memoria necesita "
                           "una corrida",
        # \~spanish LOS NOMBRES DE LAS FASES DE OTRO PROGRAMA, traducidos aqui.
        # Lo que `san_mark` guarda es una CLAVE, nunca una frase: el
        # comprobador es una libreria y no tiene por que llevar las palabras de
        # otro en el idioma de otro.  Aqui es donde ya viven las traducciones,
        # asi que aqui es donde van las palabras.  Una clave sin entrada se
        # ensena tal cual, que es una etiqueta que no escribio nadie en vez de
        # un hueco.
        "mark.vx.phase.resolve": "resolviendo el grafo de modulos",
        "mark.vx.phase.modules": "compilando los modulos",
        "mark.vx.phase.optimize": "optimizando el IR",
        "mark.vx.phase.emit": "emitiendo el .vel",
        "mark.vx.phase.link": "ensamblando y enlazando",
        "tab.sizes": "por tamano",
        "sz.note": "El mismo histograma leido de cuatro formas. Contar esconde "
                   "justo aquello para lo que esta: una casilla abarca un "
                   "RANGO de tamanos, asi que la ultima -- todo lo que pasa de "
                   "16 MiB -- sale como un punado de reservas al lado de "
                   "decenas de millones de diminutas, y en bytes puede ser la "
                   "que decide el pico.",
        "sz.draw": "dibujar",
        "sz.allocs": "reservas",
        "sz.bytes": "bytes pedidos",
        "sz.live": "sigue vivo",
        "sz.freed": "devuelto",
        "sz.bucket": "hasta",
        "sz.frees": "liberaciones",
        "sz.livemax": "lo mas vivo a la vez",
        "sz.unknown": "no consta",
        "sz.noliveaxis": "Lo que sigue vivo no esta en este volcado: un bloque "
                         "demasiado grande para el sombreado solo se reconoce "
                         "al volver por la tabla que lleva el eje, asi que sin "
                         "VESTA_ALLOC_SAN_EPOCH todo lo que pasa de las clases "
                         "pequenas nace y no muere nunca. Las columnas van "
                         "vacias y no a cero a proposito -- un cero ahi se "
                         "leeria como \"este tamano no se queda nada\".",
        "tab.time": "en el tiempo",
        "time.note": "Cuanta memoria habia viva en cada momento, y de quien "
                     "era. El arbol contesta quien reserva; esto contesta "
                     "CUANDO, que es la pregunta que decide cuanta memoria "
                     "necesita una corrida -- dos sitios que nunca estuvieron "
                     "vivos a la vez cuestan lo que cuesta el mayor, y en un "
                     "arbol se suman.",
        "time.what": "dibujar",
        "time.both": "vivo y comprometido",
        "time.onlylive": "solo vivo",
        "time.onlycomm": "solo comprometido",
        "time.top": "sitios",
        "time.stack": "apilarlos",
        "time.draw": "dibujo",
        "time.d.stack": "apiladas",
        "time.d.lines": "lineas",
        "time.d.log": "eje logaritmico",
        "time.d.pct": "100% apilado",
        "time.d.grid": "uno por banda",
        "time.d.bubbles": "burbujas",
        "time.nolog": "un cero no tiene sitio en un eje logaritmico: el suelo es {v}, y una banda que baje de ahi se dibuja sobre el",
        "time.pctaxis": "parte de lo que habia vivo",
        "time.bubbles.at": "de que estaba hecha en el corte {i}{mark} -- {n} bandas, {bytes} vivos",
        "time.bubbles.peaks": "{n} bandas, cada una en SU pico -- no fueron a la vez, asi que no se suman",
        "time.bubbles.rest": "lo demas",
        "time.nopick": "no hay nada elegido: marca una banda de abajo",
        "time.share": "parte",
        "time.hideown": "dejar fuera lo que el informe se gasto en si mismo",
        "time.filter": "filtrar sitios",
        "time.live": "vivo",
        "time.committed": "comprometido",
        "time.region": "los rangos del asignador",
        "time.slack": "guardado y sin entregar",
        "time.freespans": "...en tramos libres (volveria)",
        "time.emptychunks": "...en trozos con todo muerto (tambien)",
        "time.inclasses": "...en trozos con algo vivo repartido (no)",
        "time.churn": "movido desde el corte anterior",
        "time.cut": "corte {i} · {a} reservas",
        "time.xaxis": "{n} reservas",
        "time.back": "devolver {n}",
        "time.nomark": "sin fase",
        "time.peakhead": "en el pico: {bytes} vivos, en {mark}",
        "time.col.bytes": "vivo",
        "time.col.share": "parte",
        "time.col.growth": "forma",
        "time.group": "agrupar por",
        "time.g.site": "pila de llamadas",
        "time.g.function": "funcion",
        "time.g.file": "fichero fuente",
        "time.g.module": "modulo",
        "time.g.size": "tamano",
        "time.g.shape": "forma, la medida",
        "time.g.life": "cuanto vivieron",
        # LO QUE LAS DISTINGUE, DELANTE.  Ver la nota en la version inglesa.
        "time.life.upto": "< {n} reservas",
        "time.life.none": "no murieron",
        "time.z.in": "+",
        "time.z.out": "-",
        "time.z.all": "todo",
        "time.help": "arrastra para desplazar · rueda o los botones para "
                     "ampliar · con mayusculas se elige un tramo · doble clic "
                     "vuelve · la caja se puede hacer mas alta",
        "time.all": "todos",
        "time.none": "ninguno",
        "time.hidden": "{n} mas que el filtro no lista",
        "time.more": "{n} mas",
        "time.unzoom": "la corrida entera",
        "time.scale": "escala",
        "time.s.all": "la corrida entera",
        "time.s.picked": "lo elegido",
        "time.stacks": "{n} pilas de llamadas",
        "time.byfile": "de donde sale -- {n} fichero(s), medido corte a corte",
        "time.nofile": "(sin fichero)",
        "time.mostly": "sobre todo {file}",
        "time.nostacks": "una casilla de tamano no es un sitio: esta curva se cuenta corte a corte, no sale de repartir las pilas, asi que no hay cadena que abrir",
        "tool.theme": "tema",
        "theme.auto": "como el sistema",
        "theme.light": "claro",
        "theme.dark": "oscuro",
        "head.totals": "{allocs} reservas en {sites} sitios, "
                       "{bytes} en su maximo",
        "head.totals.run": "{allocs} reservas en {sites} sitios, "
                           "{bytes} comprometidos en toda la corrida "
                           "(acumulado)",
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
        "warn.check.cut": "{n} de {total} pilas estan CORTADAS: el recorrido "
                          "tenia mas que seguir y no donde ponerlo, asi que "
                          "cuelgan de donde se cortaron y no de quien llamo",
        "warn.check.poolfull": "{n} marcos no encontraron sitio en el almacen, "
                               "asi que las pilas que los querian son mas "
                               "cortas de lo que se camino",
        "warn.check.indexfull": "{n} granulos no encontraron sitio en el "
                                "indice: una direccion DE DENTRO de esos "
                                "bloques no se puede situar",
        "warn.check.pairsfull": "{n} liberaciones no encontraron sitio en la "
                                "tabla de pares, asi que de quien es cada cosa "
                                "es lo que CUPO, no lo que paso",
        "warn.check.noguard": "{n} reservas no consiguieron bloque con guarda "
                              "-- la tabla estaba llena -- y se sirvieron como "
                              "siempre, asi que un fallo en ellas no se caza "
                              "al instante",
        "warn.check.noshadow": "{n} trozos a los que el sistema no dio memoria "
                               "de sombreado: sus bloques no tienen veredicto "
                               "de fuga",
        "gap.level.canary": "aqui no se caza ningun desbordamiento: eso pide "
                            "el nivel de canario (VESTA_ALLOC_SAN=2)",
        "gap.level.poison": "aqui no se caza ninguna escritura despues de "
                            "liberar: eso pide el nivel de veneno "
                            "(VESTA_ALLOC_SAN=3)",
        "gap.level.guard": "aqui no se caza nada AL INSTANTE: eso pide el "
                           "nivel de guarda (VESTA_ALLOC_SAN=4)",
        "gap.level.noshadow": "aqui no hay fugas ni vidas: en el nivel de "
                              "guarda los bloques salen de paginas propias y "
                              "no llegan al sombreado que los habria vigilado",
        "chk.verdicts": "QUE ESTABA MAL: {n} hallazgos, cada uno con las pilas "
                        "que lo hicieron. Uno demostrado no es una opinion",
        "warns.summary": "{n} cosas no se pudieron cubrir y {gaps} que este "
                         "nivel no mira -- pulsa para leerlas",
        "chk.verdict.at": "reservado en",
        "cert.proven": "DEMOSTRADO",
        "cert.suspected": "SOSPECHADO",
        "file.check_verdicts": "una fila por hallazgo: que estaba mal, en que "
                               "direccion, y las pilas -- reservado, soltado, "
                               "soltado otra vez",
        "chk.owners": "de quien es cada cosa, de {pairs} pares: {one} sitios "
                      "tienen UN dueño y podrian irse a una arena propia, "
                      "{many} son compartidos y su lugar es el camino comun, y "
                      "{none} no soltaron nada, que no es ninguna de las dos",
        "col.owners": "sitios que devolvieron sus bloques: 1 quiere decir que "
                      "tiene dueño, mas de 1 que es compartido",
        "gap.partial.allocator": "esta exportacion trae solo la mitad del "
                                 "comprobador: las tablas del propio asignador "
                                 "no se escribieron, asi que lo de abajo son "
                                 "todas las pilas recorridas y NO todas las "
                                 "reservas",
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

# -*- coding: utf-8 -*-
#
# VestaVM -- Distributed Virtual Machine
#
# Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
# License: MIT (see LICENSE).  Part of the VestaVM family.
"""An export written by hand, so the tests do not need a build.

WHY SYNTHETIC AND NOT A REAL RUN.  Because a test that needs a compiled
program to produce its input is a test that stops being run: it fails on a
machine with no toolchain, it changes every time the program changes, and
whoever sees it red has to work out which of the two is broken.  Here every
number is known in advance, so a failure means the TOOL is wrong.

The awkward cases are in on purpose, because they are the ones that broke
while this was being written: a name with commas and quotes in it, the same
function allocating from two different lines, a chain with inlined frames, two
purposes, and a site with no frames at all.
"""
import os

# A name with everything that breaks a CSV reader and a column layout at once.
NASTY = ('std::pair<std::string const, std::vector<Foo, "bar">>::pair'
         '(int, char)')

SITES = [
    # id  allocs bytes  over overB large classes mask tag  tag_name        pc   foreign
    (0, 100, 6400, 0, 0, 0, 1, 2, 5, "instant/fixed", 4096, 0),
    (1, 50, 3200, 5, 320, 0, 1, 2, 5, "instant/fixed", 4128, 0),
    (2, 25, 40000, 0, 0, 3, 2, 12, 14, "long/growing", 8192, 0),
    (3, 10, 640, 0, 0, 0, 1, 4, 0, "unknown", 12288, 0),
    # A site with NO frames: the resolver knew nothing about it.  It still has
    # to appear in the tree, or the totals quietly shrink.
    (4, 7, 448, 0, 0, 0, 1, 4, 0, "unknown", 65536, 0),
    # A site in ANOTHER MODULE: the C runtime allocating inside itself.  Its
    # offset is measured from OUR base, so it is a huge meaningless number --
    # which is exactly why the column next to it exists.
    (5, 9, 450, 0, 0, 0, 1, 4, 0, "unknown", 30534573892, 1),
    # C PURO: un solo marco, en un `.c`.  Hace falta a proposito -- sin el, el
    # filtro por lenguaje no se podria probar en la direccion "solo C", que es
    # justo la mitad que se olvida.
    (6, 12, 768, 0, 0, 0, 1, 2, 0, "unknown", 16384, 0),
]

# (site, depth, inlined, function, file, line, module).  Innermost first.
#
# Sites 0 and 1 are the SAME function at two different lines: that is what
# used to come out as two sibling rows with one name.
FRAMES = [
    (0, 0, 1, NASTY, "src/util/alloc,report.cpp", 42, "util"),
    (0, 1, 0, "vx_parse_file", "src/vx/parser.c", 100, "vx"),
    (1, 0, 1, NASTY, "src/util/alloc,report.cpp", 77, "util"),
    (1, 1, 0, "vx_parse_file", "src/vx/parser.c", 130, "vx"),
    (2, 0, 0, "std::vector<int>::_M_realloc_insert<int>(int&&)",
     "include/c++/12/bits/vector.tcc", 440, "libstdc++"),
    (3, 0, 0, "main", "src/main.cpp", 10, ""),
    # The foreign one: a name and a module, and no file or line, because the
    # debug information of somebody else's library is not there to read.
    (5, 0, 0, "realloc", "", 0, "C:\\Windows\\System32\\msvcrt.dll"),
    (6, 0, 0, "vx_lexer_token", "src/vx/lexer.c", 88, "vx"),
]

# (site, bucket, upper_bytes, allocs).  Sparse, and it must SUM to each site's
# `allocs` -- that is the invariant tying the two tables together, and the one
# that fails silently if it ever drifts.
# sitio, casilla, tope, reservas, BYTES.
#
# Los bytes NO son las reservas por el tope, y no por descuido: una casilla
# abarca un rango, asi que el peso real cae dentro y nunca en el borde.  Lo que
# si cumplen es la forma que hace falta probar -- el sitio 2 son veinticinco
# reservas, la menor cuenta de la tabla, y la mayor suma de bytes con
# diferencia --, porque es justo el caso que la vista por cuentas esconde.
SITE_SIZES = [
    (0, 0, 64, 40, 1800), (0, 1, 256, 60, 9000),
    (1, 1, 256, 50, 7500),
    (2, 9, 1048576, 25, 15000000),
    (3, 0, 64, 10, 400),
    (4, 0, 64, 7, 300),
]

SIZES = [(i, limit, allocs) for i, (limit, allocs) in enumerate(
    [(64, 60), (256, 90), (1024, 25), (2048, 0), (4096, 0), (8192, 0),
     (16384, 0), (65536, 3), (262144, 0), (1048576, 0), (16777216, 0),
     (0, 0)])]

TAGS = [(5, "instant/fixed", 150), (14, "long/growing", 25), (0, "unknown", 17)]

SUMMARY = [
    ("sites", 5), ("sites_capacity", 4096), ("small_allocs", 192),
    ("small_frees", 100), ("remote_frees", 0), ("large_allocs", 3),
    ("large_frees", 0), ("chunks", 7), ("bytes_reserved", 262144),
    ("untagged", 17), ("evicted", 5), ("module_base", 140700000000000),
    ("has_symbols", 1), ("sites_without_frames", 1), ("deepest_chain", 2),
    ("frames_capacity", 64),
]


def _quote(value):
    """The same rule the exporter follows: quote when it needs it."""
    text = str(value)
    if any(c in text for c in ',"\n\r'):
        return '"' + text.replace('"', '""') + '"'
    return text


def _write(path, header, rows):
    with open(path, "w", newline="", encoding="utf-8") as handle:
        handle.write(",".join(header) + "\n")
        for row in rows:
            handle.write(",".join(_quote(v) for v in row) + "\n")


def write_export(directory, mask=True, module=True, site_sizes=True,
                 foreign=True, check=True, size_bytes=True, live_sizes=True):
    """Writes the export.

    @param mask       include `class_mask`; False writes what an OLDER build
                      wrote, which the tool still has to load.
    @param module     include the `module` column of `frames.csv`, same reason.
    @param site_sizes write `site_sizes.csv` at all; False is again an older
                      export, and its absence is not an error.
    @param foreign    include the `foreign` column of `sites.csv`.  False is an
                      export from before the allocator could serve the C
                      runtime from the inside -- back then nothing but the
                      program could allocate, so "missing" means "not foreign"
                      and not "unknown".
    @param check      write the CHECKER's half too.  False is a run without the
                      checking mode, which is the normal case and has to load
                      just as well -- the tool draws one population instead of
                      two and says nothing about it.
    @param size_bytes include the `bytes` column of `site_sizes.csv`.  False is
                      an export from before the histogram could be weighed, and
                      then the page must not offer the view at all: a bar at
                      zero and "this was not measured" cannot look the same.
    @param live_sizes fill in the live columns of `check_sizes.csv`.  False is
                      a run WITHOUT the time axis, where a block too big for
                      the shadow is born and never dies -- so what a bucket
                      keeps is unknown, and the columns come EMPTY.
    """
    if not os.path.isdir(directory):
        os.makedirs(directory)

    head = ["site_id", "allocs", "bytes", "over_allocs", "over_bytes", "large",
            "size_classes"]
    if mask:
        head.append("class_mask")
    head += ["tag", "tag_name", "pc_offset"]
    if foreign:
        head.append("foreign")
    rows = [r if mask else r[:7] + r[8:] for r in SITES]
    if not foreign:
        rows = [r[:-1] for r in rows]
    _write(os.path.join(directory, "sites.csv"), head, rows)

    head = ["site_id", "depth", "inlined", "function", "file", "line"]
    if module:
        head.append("module")
    _write(os.path.join(directory, "frames.csv"), head,
           [f if module else f[:6] for f in FRAMES])

    _write(os.path.join(directory, "sizes.csv"),
           ["bucket", "upper_bytes", "allocs"], SIZES)
    if site_sizes:
        head = ["site_id", "bucket", "upper_bytes", "allocs"]
        rows = [r[:4] for r in SITE_SIZES]
        if size_bytes:
            head.append("bytes")
            rows = list(SITE_SIZES)
        _write(os.path.join(directory, "site_sizes.csv"), head, rows)
    _write(os.path.join(directory, "tags.csv"),
           ["tag", "tag_name", "allocs"], TAGS)
    _write(os.path.join(directory, "summary.csv"), ["key", "value"], SUMMARY)

    if check:
        _write_check(directory, live_sizes)


# Lo que el COMPROBADOR exporta.  Es otra poblacion -- una fila por pila
# recorrida, no por sitio -- y hasta ahora el banco no la escribia, asi que todo
# ese lado de la pagina no lo probaba nadie: los hallazgos, de quien es cada
# cosa y los huecos por nivel se dibujaban sin que ninguna prueba los mirara.
CHECK_SITES = [
    # id, nacimientos, bytes, fuera, fuera_bytes, muertes, vivos, vivos_bytes,
    # vida_media, vida_max, cruzados, tam_min, tam_max, uso, forma, caminada,
    # marcos, cortada
    [7, 12, 480, 0, 0, 12, 0, 0, 30, 61, 0, 40, 40, "Instant", "Fixed", 1, 3, 0],
    [9, 4, 8192, 1, 4096, 2, 2, 4096, 900, 1200, 1, 2048, 4096, "Long",
     "Growing", 1, 6, 1],
    # El informe reservando para si mismo: mucho, y de nadie del programa.
    [11, 500, 64000, 0, 0, 500, 0, 0, 10, 20, 0, 128, 128, "Instant", "Fixed",
     1, 4, 0],
]
CHECK_FRAMES = [
    [7, 0, 0, 0, "operator new", "new_op.cc", 1, "vm.exe"],
    # En OTRO modulo que la pila de abajo, a proposito: con las dos en el
    # mismo, agrupar por modulo da una banda y no se distingue de estar
    # roto -- que es como estuvo, dando "(?)" para todo.
    [7, 1, 0, 0, "Parser::take", "parser.cpp", 88, "vx_lib.dll"],
    # Una pila ENTERA, de la forma que deja el recorrido por las tablas del
    # sistema: el aparato de medida por dentro, la biblioteca estandar despues,
    # el codigo del autor en medio, y como el sistema entro en el hilo por
    # fuera.  Las dos puntas estan en TODAS las pilas de una corrida real -- en
    # una compilacion de 24k lineas eran seis marcos y siete -- y sin ellas
    # aqui, el filtro que las pliega no lo probaba nadie.
    [9, 0, 0, 0, "util::detail::current_cache()",
     "util/alloc/host_allocator.h", 670, "vm.exe"],
    [9, 1, 0, 0, "new_measured(unsigned long long, void const*)",
     "vesta_alloc/src/alloc/host_allocator.cpp", 3343, "vm.exe"],
    [9, 2, 0, 0, "std::vector<int>::_M_realloc_insert<int>(int&&)",
     "include/c++/12/bits/vector.tcc", 440, "vm.exe"],
    [9, 3, 0, 0, "Emitter::grow", "emit.cpp", 140, "vm.exe"],
    [9, 4, 0, 0, "BaseThreadInitThunk", "", 0, "kernel32.dll"],
    [9, 5, 0, 0, "RtlUserThreadStart", "", 0, "ntdll.dll"],
    # EL PROPIO INFORME RESERVANDO, que en una corrida real es lo que mas
    # consume: en la compilacion de 24k lineas fueron 1.253 MB, el 63,9 % de los
    # bytes.  Corre desde la lista de salida del runtime de C, y su cadena no
    # tiene ni un marco del autor: leer DWARF y desmanglar nombres para poner el
    # informe bonito.  Tiene que quedar APARTADA y contada, no en el arbol.
    [11, 0, 0, 0, "d_append_char", "", 0, "vm.exe"],
    [11, 1, 0, 0, "util::report_alloc_sites()",
     "vesta_alloc/src/report/alloc_sites.cpp", 610, "vm.exe"],
    [11, 2, 0, 0, "run_exit_list", "", 0, "msvcrt.dll"],
    [11, 3, 0, 0, "__tmainCRTStartup", "", 0, "msvcrt.dll"],
]
CHECK_PAIRS = [[7, 9, 12, 480], [9, 9, 2, 4096]]
CHECK_VERDICTS = [
    ["proven", "released twice", "00000000deadbeef", 7, 9, 9],
    ["suspected", "released a block the checker never saw handed out",
     "00000000cafe0000", 0, 0, 0],
]
CHECK_SUMMARY = [
    ["depot_slots", "65536"], ["level", "3"],
    ["frames_stored", "12"], ["frames_that_did_not_fit", "0"],
    ["pairs_that_did_not_fit", "0"], ["verdicts", "2"],
    ["verdicts_proven", "1"], ["verdicts_not_listed", "0"],
    ["epoch_every_allocations", "100"], ["epoch_floor_bytes", "64"],
    ["epochs_recorded", "4"], ["peak_epoch", "2"],
    ["peak_mark", "test.phase.middle"], ["live_bytes_max", "9000"],
]

# EL EJE DEL TIEMPO.  Cuatro cortes que SUBEN y luego BAJAN, que es la forma
# que distingue las dos cosas que se quieren ver: el sitio 9 no suelta nunca --
# acumula -- y el 7 sube y devuelve -- trasiega --.  Con una curva monotona las
# dos se verian igual y la prueba no probaria nada.
# casilla, tope, reservas, bytes, liberaciones, devuelto, vivo, pico de vivo.
#
# La ultima casilla es lo que esto existe para enseñar: DOS reservas -- nada al
# lado de las ciento diez de la primera -- que se quedan con ochenta megabytes,
# o sea casi todo lo que sigue en pie.  Contando, esa fila es invisible.
CHECK_SIZES = [
    (0, 64, 110, 4400, 100, 4000, 400, 900),
    (1, 256, 60, 9000, 55, 8200, 800, 1200),
    (9, 1048576, 25, 15000000, 20, 12000000, 3000000, 4000000),
    (11, 0, 2, 100000000, 1, 16000000, 84000000, 84000000),
]

CHECK_EPOCHS = [
    # EL MISMO ORDEN DE COLUMNAS QUE EL FICHERO DE VERDAD, incluida `sites`,
    # que esta herramienta no lee.  Un fixture con las columnas en otro orden
    # valida una forma que no existe, y el dia que algo se lea por posicion la
    # prueba dice que va bien.
    # LA REGION EMPIEZA A CERO Y LUEGO NO.  El primer corte se toma antes de que
    # el asignador tenga rango por el que preguntar, y ese cero hay que probarlo
    # aparte: es "no se pregunto", no "no habia nada", y la curva tiene que
    # dibujarse igual.
    # Y DE LA REGION, LOS TRAMOS LIBRES.  El hueco `region - vivo` son dos
    # cosas con curas distintas, y el fixture las separa a proposito: en el
    # ultimo corte lo vivo cae y la region no, que es el trinquete que se vino
    # a medir, y ahi los tramos libres son solo una PARTE del hueco.
    # corte, marca, reservas, vivo, comprometido, region, tramos libres,
    # nacido, muerto, sitios, suelo
    [0, "test.phase.start", 100, 1000, 4000, 0, 0, 0, 1000, 0, 2, 40],
    [1, "", 200, 5000, 9000, 7000, 500, 900, 4200, 200, 2, 40],
    [2, "test.phase.middle", 300, 9000, 16000, 13000, 1200, 1800, 4100,
     100, 2, 60],
    [3, "test.phase.end", 400, 2000, 16000, 13000, 4000, 5000, 100, 7100,
     2, 20],
]
CHECK_EPOCH_SITES = [
    [0, 7, 600], [0, 9, 360],
    [1, 7, 3000], [1, 9, 1960],
    [2, 7, 5000], [2, 9, 3940],
    [3, 7, 100], [3, 9, 1880],
]
# LA MISMA CURVA REPARTIDA POR TAMANO: corte, casilla, tope, bytes vivos.
#
# No es una vista de la de arriba y el fixture lo dice: en el corte 2 la casilla
# grande tiene mas vivo que ninguna, y en las filas por sitio ese peso esta
# repartido entre dos pilas que tambien sirven casillas pequenas.  Si una se
# pudiera sacar de la otra, este fichero sobraria.
CHECK_EPOCH_SIZES = [
    [0, 0, 64, 400], [0, 9, 1048576, 560],
    [1, 0, 64, 1200], [1, 9, 1048576, 3760],
    [2, 0, 64, 900], [2, 9, 1048576, 8040],
    [3, 0, 64, 120], [3, 9, 1048576, 1860],
]
# El corte 2 es el pico, y sus filas son las que la pagina ensena primero.
CHECK_PEAK = [
    [2, "test.phase.middle", 7, 5000, 555],
    [2, "test.phase.middle", 9, 3940, 437],
]


def _write_check(directory, live_sizes=True):
    _write(os.path.join(directory, "check_sites.csv"),
           ["stack_id", "births", "bytes", "outside_births", "outside_bytes",
            "deaths", "alive_blocks", "alive_bytes", "life_avg", "life_max",
            "cross_thread", "size_min", "size_max", "use", "shape", "walked",
            "frames", "cut"], CHECK_SITES)
    # CON `module`, como los marcos del asignador: sin esa columna, agrupar
    # por modulo sobre esta poblacion da una sola banda llamada "(?)" -- un
    # control que contesta y no dice nada, que es como estuvo hasta hoy.
    _write(os.path.join(directory, "check_frames.csv"),
           ["stack_id", "frame", "depth", "inlined", "function", "file",
            "line", "module"], CHECK_FRAMES)
    _write(os.path.join(directory, "check_pairs.csv"),
           ["alloc_stack", "free_stack", "blocks", "bytes"], CHECK_PAIRS)
    _write(os.path.join(directory, "check_verdicts.csv"),
           ["certainty", "what", "address", "allocated", "released",
            "released_again"], CHECK_VERDICTS)
    _write(os.path.join(directory, "check_epochs.csv"),
           ["epoch", "mark", "allocs", "live_bytes", "committed_bytes",
            "region_bytes", "free_span_bytes", "empty_chunk_bytes",
            "born_bytes", "died_bytes", "sites", "below_floor_bytes"],
           CHECK_EPOCHS)
    _write(os.path.join(directory, "check_epoch_sites.csv"),
           ["epoch", "stack_id", "live_bytes"], CHECK_EPOCH_SITES)
    _write(os.path.join(directory, "check_epoch_sizes.csv"),
           ["epoch", "bucket", "upper_bytes", "live_bytes"],
           CHECK_EPOCH_SIZES)
    _write(os.path.join(directory, "check_peak.csv"),
           ["epoch", "mark", "stack_id", "live_bytes", "share_per_mille"],
           CHECK_PEAK)
    # LO QUE CADA TAMANO SE QUEDA.  Las dos ultimas columnas van VACIAS cuando
    # la corrida no pudo saberlo, que es lo que hay que probar: un cero ahi se
    # leeria como "este tamano no se queda nada", justo lo contrario.
    _write(os.path.join(directory, "check_sizes.csv"),
           ["bucket", "upper_bytes", "allocs", "bytes", "frees", "freed_bytes",
            "live_bytes", "live_bytes_max"],
           CHECK_SIZES if live_sizes
           else [r[:6] + ("", "") for r in CHECK_SIZES])
    _write(os.path.join(directory, "check_summary.csv"),
           ["key", "value"], CHECK_SUMMARY)


# What the tests check against, computed from the tables above so the two
# cannot drift apart.
TOTAL_ALLOCS = sum(s[1] for s in SITES)
TOTAL_BYTES = sum(s[2] for s in SITES)
TOTAL_OVER = sum(s[3] for s in SITES)

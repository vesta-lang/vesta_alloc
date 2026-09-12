# -*- coding: utf-8 -*-
#
# VestaVM -- Distributed Virtual Machine
#
# Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
# License: MIT (see LICENSE).  Part of the VestaVM family.
"""The export as a page: a tree grid, and the tables it was folded from.

WHY A GRID AND NOT A CHART.  Because what has to be shown is a call path with
numbers next to it -- the full name, the total, what the frame did on its own,
whose module it is, which file -- and that is a table with indentation, the way
every profiler worth the name shows a top-down tree.  A rectangle chart looks
impressive and answers none of it: a four-hundred-character C++ name does not
fit in a rectangle, and cutting it throws away the template arguments, which
are the part that tells two instantiations apart.

AND NOTHING IS LEFT OUT.  Under the tree go the five files of the export, whole
-- every row, every column, including columns this tool has no code for.  A
tool that shows part of a measurement makes the rest impossible to look for,
and the reader cannot tell which part is missing.

WHY JINJA2 AND NOT A STRING WITH HTML IN IT.  Because this is a page generator,
and one written by concatenating strings is a page generator that escapes
nothing: a function name carrying `<` closes a tag and quietly eats the rest of
the row.  Jinja escapes by default, the markup lives in a template a person can
read, and the stylesheet and the script are files an editor understands.
"""
import json
import os

from .i18n import LANGS, STRINGS
from .ours import is_instrument_frame, is_library_frame, is_startup_frame
from .text import CHECK_FILES, FILES
from .tree import human_bytes

TEMPLATES = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         "templates")

MISSING = ("this needs Jinja2, which is what turns the template into the "
           "page:\n"
           "    python -m pip install jinja2\n"
           "The export still reads without it: pass --text for the same tree "
           "and the same tables on the terminal.")


def _numeric(rows, column):
    """Is this column a number, judging by what is IN it.

    Asked of the data and not of a list written here, so a column added to the
    export later still lines up and still sorts as a number.  An empty column
    is not a number: aligning unknowns to the right only looks like data.
    """
    seen = False
    for row in rows:
        value = row.get(column, "")
        if value == "":
            continue
        seen = True
        try:
            float(value)
        except ValueError:
            return False
    return seen


def _column_dict(values):
    """Un diccionario para una columna, o None si no compensa.

    POR QUE.  `check_frames.csv` son 185.000 filas y su columna `file` tiene
    unos cientos de rutas distintas, cada una de ochenta caracteres: escritas
    una por fila son megabytes de la misma cadena repetida.  Guardando las
    distintas una vez y un numero por fila, la misma tabla cabe en una fraccion
    -- y sin perder nada, porque se reconstruye exacta al pintarla.

    Solo cuando PAGA: con una columna de valores casi todos distintos, el
    diccionario es la columna otra vez mas los indices, o sea peor.  El listo
    aqui seria adivinar; se mide y se decide.
    """
    seen = {}
    order = []
    for value in values:
        if value not in seen:
            seen[value] = len(order)
            order.append(value)
    # El coste de la columna tal cual, contra el del diccionario mas un indice
    # por fila (unos pocos caracteres).  Si no gana de sobra, no se toca.
    plain = sum(len(v) + 3 for v in values)
    packed = sum(len(v) + 3 for v in order) + len(values) * 4
    if packed * 1.2 >= plain:
        return None, None
    return order, [seen[v] for v in values]


def _tables(report):
    """The five files, whole, in the order they are worth reading.

    The file NAME is not translated -- it is what the file is called on disk,
    and renaming it in the page would break the only link between what is on
    screen and what is in the directory.  What is translated is what it holds.

    LAS FILAS VAN COMO DATOS, no como filas de una tabla ya escrita.  Escritas
    en el documento, cada celda costaba sus etiquetas: medido sobre una
    compilacion de 144.000 lineas, una fila de `check_frames.csv` ocupaba 441
    bytes contra los 66 de la misma fila en el CSV, y la pagina entera 104 MB
    de los que 97 eran ese envoltorio repetido dos millones de veces.  La
    pagina las pinta al abrir la pestana, y a trozos segun se baja.
    """
    out = []
    # The checker's three go LAST and only when the run had them.  Last because
    # they are a different population and reading them first invites adding
    # their numbers to the ones above; only when present because three empty
    # tables read like an export that lost them.
    names = FILES + (CHECK_FILES if report.check else ())
    for name, key in names:
        rows = report.raw.get(name, [])
        columns = report.columns.get(name, [])
        cells = [[row.get(c, "") for c in columns] for row in rows]
        words = []
        for i in range(len(columns)):
            column = [r[i] for r in cells]
            order, packed = _column_dict(column)
            words.append(order)
            if packed is not None:
                for r, index in zip(cells, packed):
                    r[i] = index
        out.append({
            "id": name.split(".")[0],
            "file": name,
            "note_key": key,
            "columns": [{"name": c,
                         "numeric": _numeric(rows, c),
                         # The two that carry code: monospaced, because a name
                         # and a path are read character by character.
                         "mono": c in ("function", "file")}
                        for c in columns],
            "rows": cells,
            # Por columna: las cadenas distintas, o nulo cuando no se empaqueto.
            "words": words,
        })
    return out


def _payload(report, tables=None):
    """The MEASUREMENT, not a tree built from it.

    @param tables lo que devuelve `_tables`, para no armarlo dos veces.  Se
                  calcula aqui si no se pasa: el payload tiene que salir
                  ENTERO de una llamada -- lo que arma la pagina y lo que
                  prueban los tests es el mismo, y si el volcado se anadiera
                  fuera, las pruebas correrian sobre unos datos que la pagina
                  no tiene.  Ya paso.\n

    WHY NOT SHIP THE TREE.  Because there is more than one tree.  The same
    sites can be read from the binary inwards or from the allocation outwards,
    and grouped first by module, by source file or by declared purpose -- and
    which of those answers "who allocated this" depends on what is being
    asked.  Precomputing them would mean eight trees in the file, most of the
    names repeated in each, and still no answer for the ninth way somebody
    wants to look at it.

    So what travels is what was measured: the frames, interned so each name is
    written ONCE, and the sites pointing into them.  The browser folds the tree
    it needs.  It is also far smaller: the same export went from 2.4 MiB of
    precooked trees to a fraction of that, because in a call tree the same
    function name appears under dozens of branches.
    """
    data = _dataset(report, report.sites, report.chain_of,
                    report.columns.get("sites.csv", []))
    # What each bucket means, so the page can label the split without a second
    # copy of the boundaries -- they come from the export, which took them
    # from the allocator.
    data["buckets"] = [[int(r["bucket"]), int(r["upper_bytes"])]
                       for r in report.sizes]
    # SI LOS BYTES POR CASILLA ESTAN SIQUIERA.  Una exportacion anterior no
    # trae la columna, y entonces la vista por bytes no se ofrece: una barra a
    # cero y "esto no se midio" no pueden parecer lo mismo.
    data["hasSizeBytes"] = 1 if any(s.size_bytes for s in report.sites) else 0
    # The checker's population, built the SAME way and kept apart.  The page
    # switches between the two with one control and folds the tree it needs
    # with the same code, which is the whole reason for giving them one shape:
    # a second renderer would drift from the first the day one of them gains a
    # column.
    if report.check:
        # DE QUIEN ES CADA SITIO, como una columna mas y no como una tabla
        # aparte.  La pregunta "este sitio se puede encaminar a una arena
        # propia" se contesta mirando su fila, no cruzando dos tablas a mano --
        # y cruzarlas a mano es lo que hace que nadie las cruce.
        #
        # Se dice el NUMERO de sitios que devuelven sus bloques, no un
        # veredicto: uno es "tiene dueño", varios es "compartido", y cero es que
        # no se vio morir ninguno -- que son tres cosas distintas y la tercera
        # no es ninguna de las dos primeras.
        owners = {}
        for (alloc, _free) in report.check.pairs:
            owners[alloc] = owners.get(alloc, 0) + 1
        for site in report.check.sites:
            site.row["owners"] = str(owners.get(site.sid, 0))
        cols = list(report.columns.get("check_sites.csv", []))
        if "owners" not in cols:
            cols.append("owners")
        check = _dataset(report, report.check.sites, report.check.chain_of,
                         cols)
        check["buckets"] = []
        # El comprobador no reparte por casilla lo que ve cada sitio: su tabla
        # es por PILA y sus tamanos son el minimo y el maximo de la vida del
        # sitio, no un histograma.  Cero explicito y no ausencia, para que el
        # conmutador no dependa de que una clave no este.
        check["hasSizeBytes"] = 0
        data["check"] = check

        # QUE ESTABA MAL, con el sitio ya NOMBRADO.  Mandar el id de la pila
        # obligaria a quien lee la pagina a cruzarla con otra tabla a mano, que
        # es lo mismo que no darlo: el nombre se resuelve aqui, una vez, donde
        # las cadenas ya estan cargadas.
        # EL EJE DEL TIEMPO, aparte del arbol y no dentro de el.  Un arbol
        # contesta QUIEN reserva y no puede contestar CUANDO: sus numeros son
        # totales de toda la corrida, y dos sitios que nunca coincidieron se
        # suman en la misma rama igual que dos que se solapan enteros.  Son dos
        # preguntas, y mezclarlas en una vista es lo que hace que la segunda no
        # se conteste nunca.
        #
        # Viaja la MEDIDA y no un dibujo, por lo mismo que el arbol: la escala,
        # que se apila y que se filtra los decide quien mira, y precocinar una
        # imagen aqui dejaria fuera la pregunta que traiga el siguiente.
        if report.check.epochs:
            data["time"] = {
                "epochs": [[e["i"], e["allocs"], e["live"], e["committed"],
                            e["born"], e["died"], e["other"], e["mark"],
                            e["region"], e["free_spans"],
                            e["empty_chunks"]]
                           for e in report.check.epochs],
                # stack_id -> [[corte, bytes], ...], disperso: un sitio solo
                # esta en los cortes en los que tenia algo.  Como pares y no
                # como objeto porque son decenas de miles de entradas y un
                # objeto por corte pesa el triple en el fichero.
                "series": {str(sid): sorted(pts.items())
                           for sid, pts in report.check.series.items()},
                # LA MISMA CURVA POR TAMANO: casilla -> [[corte, bytes], ...].
                # Una medida propia y no una vista de `series`: un sitio sirve
                # varias casillas, y `series` ademas solo lleva los sitios que
                # pasan del suelo.
                "sizes": {str(b): sorted(pts.items())
                          for b, pts in report.check.size_series.items()},
                "buckets": [[int(r["bucket"]), int(r["upper_bytes"])]
                            for r in report.sizes],
                "peak": [[p["stack"], p["bytes"], p["share"]]
                         for p in report.check.peak],
                "peakEpoch": int(report.check.summary.get("peak_epoch", 0)
                                 or 0),
                "peakMark": report.check.summary.get("peak_mark", ""),
                # El suelo por debajo del cual un sitio no se lista.  Viaja
                # porque explica el hueco entre la suma de las series y la
                # curva total: sin el, la diferencia parece un fallo.
                "floor": int(report.check.summary.get("epoch_floor_bytes", 0)
                             or 0),
                "liveMax": int(report.check.summary.get("live_bytes_max", 0)
                               or 0),
            }

        data["verdicts"] = []
        for v in report.check.verdicts:
            chain = report.check.chains.get(v["allocated"]) or []
            data["verdicts"].append({
                "certainty": v["certainty"],
                "what": v["what"],
                "address": v["address"],
                # El marco mas EXTERIOR de la cadena, que es quien llamo -- el
                # interior es siempre el propio `operator new`.
                "allocated_name": chain[-1].function if chain else "",
            })
    if tables is None:
        tables = _tables(report)
    return dict(data,
                # EL VOLCADO CRUDO, como datos.  Ver la nota de `_tables`:
                # escrito en el documento costaba el 94 % del fichero.
                raw=[{"id": t["id"], "cols": t["columns"], "rows": t["rows"],
                      "words": t["words"]} for t in tables],
                # The whole catalogue travels, not the chosen language: the
                # reader who wants the other one is not going to run the tool
                # again, and a page that has to be regenerated to change
                # language is a page that stays in the language of whoever
                # generated it.
                strings=STRINGS, langs=[list(l) for l in LANGS],
                meta=_meta(report),
                # The checker's warnings travel with the allocator's, in one
                # list.  They are about the same run, and splitting them into
                # two boxes would let a reader dismiss one box and miss that
                # half of the bytes were only weighed.
                warnings=[[k, p] for k, p in report.warnings()]
                + ([[k, p] for k, p in report.check.warnings()]
                   if report.check else [])
                # Y lo que el NIVEL pedido no produce, que no es lo mismo que
                # algo que fallara: sin esto una seccion vacia se lee como "no
                # habia nada", cuando lo cierto es "esto no se mira aqui".  Van
                # en la misma lista porque quien la lee esta preguntandose lo
                # mismo: de que NO me estoy enterando.
                + ([[k, {}] for k in report.check.missing_for_level()]
                   if report.check else [])
                # Y si el volcado viene a medias, cual mitad falta.
                + [["gap.partial." + half, {}]
                   for half in getattr(report, "partial", [])])


def _dataset(report, site_list, chain_of, site_columns):
    """One population, interned: the frames written once and the sites into
    them.

    Taken out of `_payload` so the checker's export goes through exactly this
    code and not a copy of it.  The two differ in what they measured, never in
    how they are drawn.
    """
    frames = []
    index = {}

    def intern(frame):
        key = (frame.function, frame.file, frame.line, frame.module,
               frame.inlined)
        at = index.get(key)
        if at is None:
            at = len(frames)
            index[key] = at
            frames.append([frame.function, frame.file, frame.line,
                           frame.module, 1 if frame.inlined else 0,
                           # WHETHER THIS FRAME IS LIBRARY CODE, decided HERE
                           # and not in the browser.  The page needs it to
                           # offer "only my own calls", and the rule for
                           # deciding it -- names, header paths -- is a
                           # judgement call that belongs in one place with its
                           # reasons written down (`ours.py`), not copied into
                           # JavaScript where it would drift.
                           1 if is_library_frame(frame) else 0,
                           # Y LAS DOS PUNTAS DE LA CADENA, cada una con su
                           # bit.  El instrumento de medida y la entrada del
                           # sistema al hilo no son biblioteca ni son del
                           # autor, y estan en TODAS las pilas -- ver
                           # `ours.py`.  Se marcan aqui, donde ya se decide lo
                           # demas, y se pliegan al mirar; no se borran, que
                           # es lo que convertiria "aqui empezamos a mirar" en
                           # "aqui empezo".
                           1 if is_instrument_frame(frame) else 0,
                           1 if is_startup_frame(frame) else 0])
        return at

    sites = []
    instr_allocs = instr_bytes = 0
    for site in site_list:
        chain = chain_of(site)
        # LO QUE RESERVO EL APARATO DE MEDIDA PARA SI MISMO, que es una cadena
        # ENTERA sin un solo marco del autor y con al menos uno del instrumento.
        # Las dos mitades de la regla hacen falta: una cadena que es toda
        # biblioteca -- `std::vector` creciendo sin nadie detras -- tambien
        # carece de marco del autor, y esa SI es memoria del programa.
        own = any(is_instrument_frame(f) for f in chain) and not any(
            not is_instrument_frame(f) and not is_startup_frame(f)
            and not is_library_frame(f) for f in chain)
        if own:
            instr_allocs += site.allocs
            instr_bytes += site.bytes
        sites.append({
            "id": site.sid,
            # Fuera del arbol en todos los alcances, y contado aparte.  Ver la
            # nota de `treeFor` en `tree.js`.
            "instr": 1 if own else 0,
            # Innermost first, the order the export writes them in.
            "chain": [intern(f) for f in chain],
            "allocs": site.allocs,
            "bytes": site.bytes,
            # Apart from `allocs` on purpose: what a site inherited by
            # evicting a weaker entry makes its count an upper bound, and
            # folding the two together would hide that.
            "over": site.over,
            "overBytes": site.over_bytes,
            "tag": site.tag,
            "large": site.large,
            # In two halves of 32 bits because a JavaScript number stops being
            # exact above 2^53, and these are 64 bits of flags: OR-ing them as
            # one number would silently drop the top classes -- the big ones,
            # which are the ones worth seeing.
            "mask": [site.mask & 0xFFFFFFFF, (site.mask >> 32) & 0xFFFFFFFF],
            # bucket -> allocations, sparse.  Summed up the tree so a branch
            # can answer "many small or a few large", which the process-wide
            # histogram cannot: a global split cannot be handed back out to
            # the sites that formed it.
            "sizes": {str(b): n for b, n in sorted(site.sizes.items())},
            # Y los BYTES de cada casilla, que es la otra mitad de la misma
            # pregunta: por cuenta, la ultima casilla de una corrida de verdad
            # son treinta y tres reservas entre noventa millones, y por bytes es
            # la octava parte de todo lo que el programa pidio.
            "sizeBytes": {str(b): n
                          for b, n in sorted(site.size_bytes.items())},
            "off": "0x%x" % site.pc,
            # In a module that is NOT the program: the C runtime, a system
            # library.  A FACT from the export and not a guess -- from out here
            # a shared library and a logical module of the project look the
            # same.  The page uses it for the "only my own calls" filter, and
            # it also explains the offset: for a foreign site the displacement
            # is measured from OUR base, so it means nothing.
            "foreign": 1 if site.foreign else 0,
            # The whole row, so a tree row can be opened into the measurement
            # it was folded from without another lookup table.
            "row": [site.row.get(c, "") for c in site_columns],
        })
    return {"frames": frames, "sites": sites, "siteCols": site_columns,
            # Los totales son los DEL PROGRAMA: lo que el informe se gasto en si
            # mismo viaja aparte, para que un porcentaje del arbol sea un
            # porcentaje de la memoria del programa y no de la suma de las dos
            # cosas.  Sin separarlos, el sitio mas pesado de un programa salia
            # con un 8 % que en realidad era un 22 %.
            "totals": {"allocs": sum(s["allocs"] for s in sites) - instr_allocs,
                       "bytes": sum(s["bytes"] for s in sites) - instr_bytes,
                       "instrAllocs": instr_allocs,
                       "instrBytes": instr_bytes}}


def _meta(report):
    """The totals as NUMBERS, for the page to word in whichever language.

    Formatted here they would be one language for ever, and the reader who
    switches would get a translated page with an English headline on it.
    """
    summary = report.summary
    # LO QUE EL PROCESO NECESITO, si el volcado lo trae.  El titular decia
    # "comprometido" y sacaba `bytes_reserved`, que es ACUMULADO -- un rango
    # reusado vuelve a contar --, asi que anunciaba una cifra que el programa
    # nunca llego a tener: medido, 3.328 MiB de titular con un proceso que no
    # paso de 2.374.  Un volcado viejo no trae la clave nueva; entonces se cae
    # al acumulado y la etiqueta lo dice.
    pico = int(summary.get("process_committed_peak", 0) or 0)
    acumulado = int(summary.get("committed_over_the_run",
                                summary.get("bytes_reserved", 0)) or 0)
    return {
        "sites": summary.get("sites", "?"),
        "allocs": summary.get("small_allocs", "?"),
        "bytes": human_bytes(pico if pico else acumulado),
        "bytes_key": "sub.peak" if pico else "sub.overrun",
        "unresolved": summary.get("sites_without_frames", "0"),
    }


def _read(name):
    with open(os.path.join(TEMPLATES, name), "r", encoding="utf-8") as handle:
        return handle.read()


def write_page(report, build_tree, out_path, title):
    """Writes the self-contained page.  Returns its size in bytes.

    `build_tree` is not used any more -- the page folds its own trees now --
    and stays in the signature because the caller passes it and because the
    terminal view still needs it.  It is one argument, not a reason to make
    two call shapes for the same thing.
    """
    from jinja2 import Environment, FileSystemLoader
    from markupsafe import Markup

    # `autoescape=True` and not by extension: this page is fed function names
    # straight out of a symbol table, and one containing `<` would close a tag
    # and swallow the rest of the row.  Escaping is the default and the three
    # things that must NOT be escaped are marked one by one, right here.
    env = Environment(loader=FileSystemLoader(TEMPLATES), autoescape=True,
                      trim_blocks=True, lstrip_blocks=False)
    tables = _tables(report)
    data = json.dumps(_payload(report, tables), separators=(",", ":"))
    # A `</script>` inside a string would end the block early, and the rest of
    # the data would be parsed as HTML.  It cannot happen with our own names,
    # but the data comes from whatever was linked, so it is not left to luck.
    data = data.replace("</", "<\\/")

    page = env.get_template("page.html.j2").render(
        title=title,
        langs=LANGS,
        tables=tables,
        # Whether to offer the population switch at all.  Not a disabled
        # control: one that offers something the file does not carry is a
        # promise the page cannot keep.
        has_check=bool(report.check),
        raw_rows=sum(len(t["rows"]) for t in tables),
        css=Markup(_read("page.css")),
        # ORDER MATTERS: `page.js` calls into `tables.js` while it starts up,
        # and a classic script that is not there yet is not an error you see --
        # the tree draws, the tables underneath just never sort.
        # ORDER MATTERS: `code.js` defines the escaping and the symbol
        # painting that the others use, and `page.js` calls into all of them
        # while it starts up.  A classic script that is not there yet is not
        # an error you see -- the page just comes up half-drawn.
        scripts=[Markup(_read("code.js")), Markup(_read("i18n.js")),
                 Markup(_read("tables.js")), Markup(_read("tree.js")),
                 Markup(_read("link.js")), Markup(_read("detail.js")),
                 # El eje del tiempo ANTES de `page.js`, por lo mismo que los
                 # demas: `page.js` lo arranca mientras se monta, y un script
                 # clasico que aun no esta no da un error que se vea -- la
                 # pestana sale vacia y parece que la corrida no llevaba eje.
                 Markup(_read("time.js")),
                 Markup(_read("page.js"))],
        data=Markup(data))
    with open(out_path, "w", encoding="utf-8") as handle:
        handle.write(page)
    return os.path.getsize(out_path)

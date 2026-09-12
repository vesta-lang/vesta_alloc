# -*- coding: utf-8 -*-
#
# VestaVM -- Distributed Virtual Machine
#
# Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
# License: MIT (see LICENSE).  Part of the VestaVM family.
"""The whole export on a terminal, using the standard library alone.

NOT A REDUCED VERSION OF THE PAGE.  It prints the same thing: the tree, and
then every file of the export with every column and every row.  The machine a
memory problem shows up on is usually not the one with a browser, and a mode
that quietly shows less would make the difference between the two impossible
to see -- someone would read a short table here and conclude the export was
short.

Columns are padded, never cut.  A C++ symbol runs long and the tail is where
the template arguments are; trimming it to keep the columns straight throws
away the part that separates two instantiations of the same template.
"""
from .i18n import t
from .tree import build_tree, human_bytes, print_tree

# The order they are printed in, and the catalogue key that says what each one
# is.  The description is printed with it: a column called `over_allocs` means
# nothing to someone looking at this for the first time.
FILES = (
    ("sites.csv", "file.sites"),
    ("frames.csv", "file.frames"),
    ("sizes.csv", "file.sizes"),
    ("site_sizes.csv", "file.site_sizes"),
    ("tags.csv", "file.tags"),
    ("summary.csv", "file.summary"),
)

# The checker's own three, printed only when the run had it.  Kept in a second
# tuple rather than folded into the one above so that a run WITHOUT the checker
# does not end with three empty tables -- which reads like an export that lost
# them, not like a build that never wrote them.
CHECK_FILES = (
    ("check_sites.csv", "file.check_sites"),
    ("check_frames.csv", "file.check_frames"),
    ("check_pairs.csv", "file.check_pairs"),
    ("check_verdicts.csv", "file.check_verdicts"),
    # Los tres del eje del tiempo.  Van juntos y despues de los de arriba
    # porque se leen en ese orden: primero QUIEN reserva, y solo cuando eso ya
    # no contesta, CUANDO.
    ("check_sizes.csv", "file.check_sizes"),
    ("check_epochs.csv", "file.check_epochs"),
    ("check_epoch_sites.csv", "file.check_epoch_sites"),
    ("check_peak.csv", "file.check_peak"),
    ("check_summary.csv", "file.check_summary"),
)


# How wide a column is allowed to get.  One four-hundred-character symbol in a
# table makes its column four hundred wide and pushes everything after it off
# the screen for every OTHER row.  A value longer than this is still printed
# WHOLE -- it just runs over its column on its own line, which costs the
# alignment of that one row instead of the alignment of all of them.
MAX_COLUMN = 60


def looks_numeric(text):
    """Si la celda es un numero escrito.  Solo para decidir por que lado pega."""
    if not text:
        return False
    try:
        float(text)
        return True
    except ValueError:
        return False


def print_grid(titles, rows, out, align=False):
    """Every row, every column, aligned.  Rows are lists of strings.

    The width of a column is the width of its widest VALUE -- computed, not
    guessed -- so nothing has to be shortened to fit a number decided in
    advance.  Capped at `MAX_COLUMN`, which is a limit on the COLUMN and never
    on the value.

    Con `align`, una columna en la que TODA celda con algo es un numero se pega
    a la derecha.  Es la diferencia entre poder comparar una columna de MiB de
    un vistazo y tener que leerla cifra a cifra; y se pide, no se impone,
    porque las tablas del informe llevan anos saliendo por la izquierda y
    cambiarlas seria cambiar lo que alguien ya sabe leer.
    """
    if not rows:
        out.write("    (no rows)\n")
        return
    width = [len(c) for c in titles]
    for cells in rows:
        for i, cell in enumerate(cells):
            if len(cell) > width[i]:
                width[i] = min(len(cell), MAX_COLUMN)
    right = [False] * len(titles)
    if align:
        for i in range(len(titles)):
            column = [r[i] for r in rows if i < len(r) and r[i]]
            right[i] = bool(column) and all(looks_numeric(c) for c in column)

    def laid_out(cells):
        return "  ".join(c.rjust(width[i]) if right[i] else c.ljust(width[i])
                         for i, c in enumerate(cells))

    out.write("    %s\n" % laid_out(titles))
    out.write("    %s\n" % "  ".join("-" * w for w in width))
    for cells in rows:
        out.write("    %s\n" % laid_out(cells))


def print_table(columns, rows, out):
    """Lo mismo, con las filas como diccionarios: lo que leen las tablas."""
    print_grid(columns,
               [[str(row.get(c, "")) for c in columns] for row in rows], out)


def print_check(report, limit, out, lang="en"):
    """The checker's tree, when the run had one.

    A SECOND tree and not more branches of the first, because the two measure
    different populations: the allocator's export covers every block through
    one return address, and this one covers what fits the shadow through a
    walked stack.  Drawing them as one would add up numbers that do not mean
    the same thing, and the sum would look more certain than either.

    Silence when there is no export: a run without the checker is the normal
    case, and a warning for it would train the reader to skip warnings.
    """
    check = report.check
    if check is None:
        return
    tree = build_tree(check, True, "", None)
    out.write("\n== %s ==\n\n%s\n\n" % (t(lang, "chk.title"),
                                        t(lang, "chk.sub")))
    weighed = sum(s.weighed_bytes for s in check.sites)
    alive = sum(s.alive_bytes for s in check.sites)
    out.write("%s moved, %s in %d stacks; %s only weighed, %s alive at exit\n\n"
              % (human_bytes(tree.bytes), "{:,}".format(tree.allocs),
                 len(check.sites), human_bytes(weighed), human_bytes(alive)))
    for key, params in check.warnings():
        out.write("WARNING: %s\n" % t(lang, key, **params))
    # Y lo que el NIVEL no produce.  Un aviso dice que algo se intento y no
    # cupo; esto dice que ni se mira, porque el nivel pedido no lo hace.  Sin
    # decirlo, una seccion vacia se lee como "no habia nada".
    for key in check.missing_for_level():
        out.write("WARNING: %s\n" % t(lang, key))

    # DE QUIEN ES CADA COSA, resumido: la pregunta que abre el encaminado.  Un
    # sitio cuyos bloques vuelven todos por UN sitio tiene dueño y podria irse a
    # una arena propia; por varios, es compartido y su lugar es el camino comun.
    # Se dan las tres cuentas y no un veredicto, porque "no se vio morir
    # ninguno" no es ninguna de las otras dos.
    if check.pairs:
        owners = {}
        for (alloc, _free) in check.pairs:
            owners[alloc] = owners.get(alloc, 0) + 1
        one = sum(1 for n in owners.values() if n == 1)
        many = sum(1 for n in owners.values() if n > 1)
        none = len(check.sites) - len(owners)
        out.write("\n%s\n" % t(lang, "chk.owners", one=one, many=many,
                               none=none, pairs=len(check.pairs)))

    # QUE ESTABA MAL, primero de todo lo que sigue.  Un hallazgo importa mas que
    # cualquier reparto, y esconderlo detras del arbol seria dejarlo donde nadie
    # baja.  Con su direccion y el sitio que lo reservo, que es la pregunta
    # siguiente en cuanto se lee la linea.
    if check.verdicts:
        out.write("\n%s\n" % t(lang, "chk.verdicts", n=len(check.verdicts)))
        for v in check.verdicts:
            out.write("  %-9s %s  en %s\n"
                      % (v["certainty"].upper(), v["what"], v["address"]))
            chain = check.chains.get(v["allocated"])
            if chain:
                out.write("      reservado en: %s\n" % chain[-1].function)
    out.write("\n")
    print_tree(tree, tree.allocs, 0, limit, out)


def print_header(report, tree, out, lang="en"):
    summary = report.summary
    # El maximo que el proceso llego a tener, no el acumulado.  Mismo motivo
    # que en la pagina: el acumulado cuenta un rango reusado otra vez, asi que
    # anuncia una cifra que el programa nunca tuvo.  Sin la clave nueva (un
    # volcado viejo) se cae al acumulado y la etiqueta lo dice.
    pico = int(summary.get("process_committed_peak", 0) or 0)
    acumulado = int(summary.get("committed_over_the_run",
                                summary.get("bytes_reserved", 0)) or 0)
    out.write("\n%s\n" % t(lang, "head.totals" if pico else "head.totals.run",
                           allocs="{:,}".format(tree.allocs),
                           sites="{:,}".format(len(report.sites)),
                           bytes=human_bytes(pico if pico else acumulado)))
    for key, params in report.warnings():
        out.write("WARNING: %s\n" % t(lang, key, **params))
    # Y si el volcado viene a medias, cual mitad falta.  Sin esto la cabecera
    # dice "0 reservas en 0 sitios", que se lee como "este programa no reservo
    # nada" cuando lo cierto es "esa mitad no se exporto".
    for half in getattr(report, "partial", []):
        out.write("WARNING: %s\n" % t(lang, "gap.partial." + half))


def print_report(report, tree, limit, out, tables=True, lang="en"):
    """The tree first, then the export itself.

    The tree is what answers the question; the tables underneath are what it
    was folded from, so any row of it can be checked instead of believed.
    """
    print_header(report, tree, out, lang)
    out.write("\n== %s ==\n\n" % t(lang, "head.tree"))
    print_tree(tree, tree.allocs, 0, limit, out)
    print_check(report, limit, out, lang)
    if not tables:
        return
    for name, key in FILES + (CHECK_FILES if report.check else ()):
        rows = report.raw.get(name, [])
        columns = report.columns.get(name, [])
        out.write("\n== %s -- %s (%s) ==\n\n"
                  % (name, t(lang, key), t(lang, "raw.rows", n=len(rows))))
        print_table(columns, rows, out)
    out.write("\n")


# --------------------------------------------------------------------------
#  La vista del modo consulta
# --------------------------------------------------------------------------


def cell_text(value):
    """Un valor de la consulta como se escribe.

    NULO SE ESCRIBE VACIO, nunca "None" ni cero.  Es la misma regla que sigue
    la exportacion, y romperla aqui convertiria "no se midio" en una medida en
    el ultimo paso del camino.
    """
    if value is None:
        return ""
    if isinstance(value, bool):
        return "si" if value else "no"
    if isinstance(value, float):
        # Sin ceros de relleno: una columna de MiB con `.0` a la derecha en
        # media tabla se lee peor y no dice nada mas.
        return ("%.2f" % value).rstrip("0").rstrip(".")
    return str(value)


def print_query(result, out, limit=0, csv_out=False):
    """Lo que salio de una consulta, y lo que no se pudo mirar.

    El pie NO es un adorno: una consulta que aparta filas -- porque la
    condicion no se pudo juzgar, o porque se corto la lista -- y no lo dice
    entrega una tabla que parece completa.
    """
    rows = [[cell_text(v) for v in row] for row in result.rows]
    shown = rows if not limit else rows[:limit]

    if csv_out:
        # Para encadenar con otra cosa.  Se citan las celdas con coma o
        # comillas, que en una tabla de simbolos de C++ son casi todas.
        out.write(",".join(quoted(c) for c in result.titles) + "\n")
        for cells in shown:
            out.write(",".join(quoted(c) for c in cells) + "\n")
    else:
        print_grid(result.titles, shown, out, align=True)

    out.write("\n    %d fila(s)" % result.total)
    if limit and len(rows) > limit:
        out.write(", %d sin mostrar (sube `--rows`)" % (len(rows) - limit))
    if result.skipped:
        out.write("; %d no cumplian" % result.skipped)
    if result.unsure:
        out.write("; %d NO SE PUDIERON JUZGAR (una celda sin medir)"
                  % result.unsure)
    out.write("\n")
    for title in sorted(result.nulls):
        out.write("    `%s` venia sin medir en %d fila(s) -- vacio no es cero"
                  "\n" % (title, result.nulls[title]))
    for title in result.unsummable:
        out.write("    `%s` no se puede sumar: no es un numero.  Sale en "
                  "blanco\n" % title)


def quoted(text):
    """Una celda para un CSV, citada solo si hace falta."""
    if any(c in text for c in ',"\n'):
        return '"%s"' % text.replace('"', '""')
    return text


def print_tables(report, out, pseudo_of):
    """Que tablas hay, con cuantas filas y que columnas.

    Incluidas las PEGADAS, que es lo que hace que se puedan usar: una columna
    que existe y no esta listada no la escribe nadie.
    """
    for name in sorted(report.raw):
        columns = report.columns.get(name, [])
        rows = report.raw.get(name, [])
        if not columns and not rows:
            continue
        out.write("%-24s %7d fila(s)  %s\n"
                  % (name, len(rows), ", ".join(columns)))
        extra = pseudo_of(report, name)
        if extra:
            out.write("%-24s %7s   + pegadas: %s\n"
                      % ("", "", ", ".join(extra)))


def print_grep(hits, out, limit=0):
    """Donde aparecio, tabla por tabla."""
    if not hits:
        out.write("no aparece en ninguna tabla\n")
        return
    shown = hits if not limit else hits[:limit]
    last = None
    for name, index, column, row in shown:
        if name != last:
            out.write("\n== %s ==\n" % name)
            last = name
        out.write("  [%d] %s=%s\n" % (index, column, row.get(column, "")))
        out.write("      %s\n"
                  % "  ".join("%s=%s" % (k, v) for k, v in row.items()
                              if k is not None and v and k != column))
    out.write("\n%d coincidencia(s)" % len(hits))
    if limit and len(hits) > limit:
        out.write(", %d sin mostrar (sube `--rows`)" % (len(hits) - limit))
    out.write("\n")

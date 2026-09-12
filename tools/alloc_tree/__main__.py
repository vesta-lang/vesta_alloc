# -*- coding: utf-8 -*-
#
# VestaVM -- Distributed Virtual Machine
#
# Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
# License: MIT (see LICENSE).  Part of the VestaVM family.
"""Where the allocations are born, as a tree you can walk.

    VESTA_HOST_ALLOC_SITES=1 VESTA_HOST_ALLOC_CSV=/tmp/run  ./your_program
    python -m alloc_tree /tmp/run

The allocator can print a report, and that report answers "what allocates
most".  It cannot answer the next question, which is the one that leads
somewhere: whose code asked for it, through which path, and what the picture
looks like once a branch is folded away.  That is walking and drilling in -- a
tool's job, not a printout's.
"""
import argparse
import os
import re
import sys
import webbrowser

from . import query
from .i18n import LANGS, t
from .report import Report
from .symbols import add_names
from .text import print_grep, print_query, print_report, print_tables
from .tree import build_tree


def parse_args(argv):
    parser = argparse.ArgumentParser(
        prog="alloc_tree", description=__doc__.strip().splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("directory",
                        help="what VESTA_HOST_ALLOC_CSV wrote")
    parser.add_argument("--text", "-t", action="store_true",
                        help="the same tree AND the same tables on the "
                             "terminal, with no dependencies at all")
    parser.add_argument("--tree-only", action="store_true",
                        help="in --text, print the tree and not the five "
                             "tables under it")
    parser.add_argument("--bottom-up", action="store_true",
                        help="start where the allocation happens instead of "
                             "at the function that exists in the binary")
    # The same groupings the page offers, so the two views answer the same
    # questions.  A terminal mode that could only do one of them would make
    # the difference between the two look like a difference in the data.
    parser.add_argument("--group", choices=("stack", "module", "file",
                                            "purpose"), default="stack",
                        help="put a level on TOP of the call stack: whose "
                             "module it is, which source file, or what it was "
                             "declared for (default: stack, no grouping)")
    parser.add_argument("--names", metavar="BINARY",
                        help="put names on the offsets of a stripped build, "
                             "with addr2line and the unstripped binary of "
                             "that SAME build")
    parser.add_argument("--out", metavar="FILE",
                        help="where to write the page "
                             "(default: alloc_tree.html inside the export)")
    parser.add_argument("--no-open", action="store_true",
                        help="write the page and do not launch a browser")
    # Only for the TERMINAL view.  The page carries every language inside and
    # switches without reloading, because whoever reads it is not the one who
    # ran this command.
    parser.add_argument("--lang", choices=[code for code, _ in LANGS],
                        default="en",
                        help="language of the --text output (the page has a "
                             "selector of its own)")
    # Zero by default, and that is the point: the whole tree comes out unless
    # somebody ASKS for it to be folded.  A default that hides the small
    # branches hides them from the person who does not know the flag exists,
    # and a branch that is small in allocations can be the one holding the
    # bytes.
    parser.add_argument("--limit", type=float, default=0.0, metavar="PCT",
                        help="in --text, fold branches under this %% away, "
                             "saying how many (default: 0, print everything)")
    # -----------------------------------------------------------------------
    #  Preguntarle a las tablas
    # -----------------------------------------------------------------------
    #
    # Un grupo aparte porque es otro MODO, no un ajuste del arbol: contesta una
    # pregunta concreta y sale.  Ver `query.py` para por que esto no se deja a
    # un `python -c` escrito a mano cada vez.
    ask = parser.add_argument_group(
        "preguntar",
        "consultar las tablas en vez de dibujarlas.  Ejemplos:\n"
        "  --tables\n"
        "  --table epochs --cols 'epoch,mark,mib(region_bytes-live_bytes)"
        " as holgura' --sort=-holgura\n"
        "  --table sites --where 'module==\"ir\"' --cols 'func,bytes,allocs'"
        " --sort=-bytes\n"
        "  --table sites --group-by module --cols bytes,allocs"
        " --sort=-bytes\n"
        "  --grep dce_impl\n"
        "\n"
        "OJO con el orden descendente: va PEGADO con `=`.  `--sort -bytes`\n"
        "separado lo lee argparse como una opcion que no existe.")
    ask.add_argument("--tables", action="store_true",
                     help="que tablas hay, con sus columnas y las que se "
                          "pegan al cruzar los marcos")
    ask.add_argument("--table", metavar="NAME",
                     help="consultar esa tabla; vale la abreviatura "
                          "(`epochs` por `check_epochs.csv`)")
    ask.add_argument("--where", metavar="EXPR",
                     help="quedarse con las filas que cumplan la expresion")
    ask.add_argument("--cols", metavar="EXPRS",
                     help="columnas de salida, separadas por comas, con "
                          "`expresion as nombre` (por defecto, todas)")
    ask.add_argument("--group-by", metavar="EXPR",
                     help="agrupar por eso y SUMAR las columnas numericas")
    ask.add_argument("--sort", metavar="COL",
                     help="ordenar por una columna de SALIDA; para invertir, "
                          "`--sort=-COL` pegado con `=`")
    ask.add_argument("--rows", type=int, default=40, metavar="N",
                     help="cuantas filas mostrar (0 = todas; por defecto 40)")
    ask.add_argument("--csv", action="store_true",
                     help="sacar la consulta como CSV, para encadenarla")
    ask.add_argument("--grep", metavar="PATTERN",
                     help="buscar esa expresion regular en TODAS las tablas")

    # NO HAY OPCION PARA "solo mis llamadas", Y ES A PROPOSITO.  Filtrar es
    # una forma de MIRAR, no de exportar: quien lee el informe cambia de idea
    # tres veces mientras lo mira, y una bandera de linea de ordenes obliga a
    # volver a generarlo cada vez -- con el riesgo de acabar comparando dos
    # informes distintos sin darse cuenta.
    #
    # La pagina lleva SIEMPRE todo lo medido y el filtro dentro, donde se
    # enciende y se apaga sin perder nada.  Ver `ours.py` para el criterio.
    return parser.parse_args(argv)


def ask_tables(report, args):
    """El modo consulta: `--tables`, `--table ...` y `--grep`.

    Los errores de una consulta -- una tabla que no existe, una columna mal
    escrita, una expresion que no compila -- se cuentan como lo que son: el
    usuario se equivoco al preguntar, no la herramienta al leer.  Se dice QUE
    esta mal y se sale con 2, igual que un argumento invalido.
    """
    if args.tables:
        print_tables(report, sys.stdout, query.pseudo_columns)
        return 0

    if args.grep:
        try:
            hits = query.grep(report, args.grep)
        except re.error as exc:
            print("esa expresion regular no vale: %s" % exc)
            return 2
        print_grep(hits, sys.stdout, args.rows)
        return 0

    # La tabla se resuelve APARTE, y antes: si falla, el manejador de abajo no
    # tendria `table` para decir que columnas hay, y en vez del error de
    # verdad saldria otro sobre una variable.
    try:
        table = query.resolve_table(report, args.table)
    except ValueError as exc:
        print("%s" % exc)
        return 2

    try:
        result = query.select(report, table, where=args.where,
                              cols=args.cols, group_by=args.group_by)
        if args.sort:
            query.sort_rows(result, args.sort)
    except ValueError as exc:
        print("%s" % exc)
        return 2
    except SyntaxError as exc:
        print("esa expresion no se entiende: %s" % exc)
        return 2
    except NameError as exc:
        # Lo mas facil de equivocar: una columna que no es de esa tabla.  El
        # mensaje trae las que SI hay, que es lo que hace falta para arreglarlo
        # sin tener que ir a mirar la cabecera del fichero.
        columns = list(report.columns.get(table, []))
        columns.extend(query.pseudo_columns(report, table))
        print("%s.  En `%s` hay: %s" % (exc, table, ", ".join(columns)))
        return 2
    print_query(result, sys.stdout, args.rows, args.csv)
    return 0


def main(argv=None):
    args = parse_args(sys.argv[1:] if argv is None else argv)
    if not os.path.isdir(args.directory):
        print("not a directory: %s" % args.directory)
        return 2

    try:
        report = Report(args.directory)
    except IOError as exc:
        print("could not load %s: %s" % (args.directory, exc))
        return 1

    if args.names:
        try:
            named, asked = add_names(report, args.names)
            print("named %d of %d offsets from %s"
                  % (named, asked, os.path.basename(args.names)))
            if named < asked:
                print("  the rest are not in that binary -- is it the same "
                      "build?  Offsets from one build mean nothing in "
                      "another.")
        except IOError as exc:
            print("addr2line: %s" % exc)
            return 1

    # Preguntar va ANTES de dibujar, y sale: es otro modo.  Y despues de
    # `--names` a proposito, para que la columna `func` de una consulta traiga
    # nombres cuando se hayan podido poner.
    if args.tables or args.table or args.grep:
        return ask_tables(report, args)

    if args.text:
        # The warnings are printed by `print_report` itself, right under the
        # totals, so that the terminal output reads the same as the page.
        tree = build_tree(report, not args.bottom_up,
                          group=None if args.group == "stack" else args.group)
        print_report(report, tree, args.limit, sys.stdout,
                     tables=not args.tree_only, lang=args.lang)
        return 0

    # What could NOT be done goes first, and always: a tree with no names looks
    # like a program that allocates from nowhere.
    for key, params in report.warnings():
        print("WARNING: %s" % t(args.lang, key, **params))

    try:
        from .page import write_page
    except ImportError:
        from .page import MISSING          # noqa: F401 -- solo para el mensaje
        print(MISSING)
        return 2

    out = args.out or os.path.join(args.directory, "alloc_tree.html")
    try:
        size = write_page(report, build_tree, out,
                          "vesta_alloc -- " + os.path.abspath(args.directory))
    except ImportError:
        from .page import MISSING
        print(MISSING)
        return 2

    print("wrote %s (%.1f KiB)" % (out, size / 1024.0))
    if not args.no_open:
        webbrowser.open("file://" + os.path.abspath(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())

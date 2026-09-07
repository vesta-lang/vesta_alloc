# -*- coding: utf-8 -*-
#
# VestaVM -- Distributed Virtual Machine
#
# Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
# License: MIT (see LICENSE).  Part of the VestaVM family.
"""What the tool must not get wrong about the measurement.

The checks here are the ones that broke while this was being written, and each
one of them broke QUIETLY -- a tree that had lost allocations still drew, a
function split across two rows still looked like a tree, a half-translated
page still opened.  That is why they are tests and not a careful reading.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))          # tools/

import fixtures                                     # noqa: E402
from alloc_tree.i18n import LANGS, STRINGS, t       # noqa: E402
from alloc_tree.report import Report                # noqa: E402
from alloc_tree.tree import build_tree              # noqa: E402

GROUPINGS = (None, "module", "file", "purpose")


def load(tmp, **kw):
    directory = os.path.join(tmp, "export")
    fixtures.write_export(directory, **kw)
    return Report(directory)


def check_loads_everything(report, say):
    say(len(report.sites) == len(fixtures.SITES),
        "carga todos los sitios")
    say(sum(len(c) for c in report.frames.values()) == len(fixtures.FRAMES),
        "y todos los marcos")
    # Every column, including the ones the tool has no code for: dropping one
    # here would make it impossible to look for later.
    say(report.columns["sites.csv"][-1] == "foreign" and
        len(report.columns["sites.csv"]) == 12,
        "guarda las doce columnas de sites.csv, no solo las que usa")
    # De OTRO MODULO, y como dato del volcado y no como suposicion: desde aqui
    # una biblioteca del sistema y un modulo logico del proyecto se escriben
    # igual en la columna `module`.
    say([s.sid for s in report.sites if s.foreign] == [5],
        "y sabe cual es de otro modulo, porque el volcado lo DICE")
    say(len(report.raw["summary.csv"]) == len(fixtures.SUMMARY),
        "y summary.csv entero")
    say(report.sites[0].row["tag_name"] == "instant/fixed",
        "la fila cruda viaja con el sitio")


def check_nothing_is_lost(report, say):
    """The one that matters: folding must not lose allocations.

    A tree that has lost some still draws, and its percentages still add up to
    a hundred -- of a smaller total.  Nothing on screen says so.
    """
    for top_down in (True, False):
        for group in GROUPINGS:
            tree = build_tree(report, top_down, group=group)
            ok = (tree.allocs == fixtures.TOTAL_ALLOCS and
                  tree.bytes == fixtures.TOTAL_BYTES and
                  tree.over == fixtures.TOTAL_OVER)
            say(ok, "no se pierde nada al plegar (%s, %s)"
                % ("de fuera" if top_down else "de dentro", group or "pila"))

    # A site the resolver knew nothing about still has to be in there.
    tree = build_tree(report, True)
    names = []

    def walk(node):
        for kid in node.sorted_children():
            names.append(kid.frame.function)
            walk(kid)
    walk(tree)
    say(any(n.startswith("+0x") for n in names),
        "un sitio sin marcos sale igual, como desplazamiento")


def check_functions_group(report, say):
    """The same function at two lines is ONE node, not two siblings."""
    tree = build_tree(report, True)
    top = tree.sorted_children()
    outer = [k for k in top if k.frame.function == "vx_parse_file"]
    say(len(outer) == 1, "la misma funcion en dos lineas es UN nodo")
    if outer:
        node = outer[0]
        say(node.allocs == 150,
            "y suma lo de sus dos sitios (100 + 50)")
        say(len(node.sorted_children()) == 1,
            "con un solo hijo, no dos copias del mismo nombre")


def check_purposes_travel(report, say):
    """The purposes of a branch add up to the branch."""
    for group in GROUPINGS:
        tree = build_tree(report, True, group=group)
        bad = []

        def walk(node):
            if sum(node.tags.values()) != node.allocs:
                bad.append(node.frame.function)
            for kid in node.sorted_children():
                walk(kid)
        walk(tree)
        say(not bad, "los propositos suman el nodo (%s)" % (group or "pila"))

    tree = build_tree(report, True, group="purpose")
    heads = sorted(k.frame.function for k in tree.sorted_children())
    say(heads == ["instant/fixed", "long/growing", "unknown"],
        "agrupar por proposito da un nivel por proposito")


def check_sizes_per_site(report, say):
    """The per-site split has to sum to the site, and to the branch."""
    by_site = {s.sid: s for s in report.sites}
    ok = True
    for sid, hist in ((s.sid, s.sizes) for s in report.sites):
        if hist and sum(hist.values()) != by_site[sid].allocs:
            ok = False
    say(ok, "el reparto de tamanos de cada sitio suma sus reservas")

    # And folded into the tree, the root's split must be the sum of them all:
    # this is the whole point of recording it per site.
    tree = build_tree(report, True)
    total = {}
    for site in report.sites:
        for b, n in site.sizes.items():
            total[b] = total.get(b, 0) + n
    say(sum(total.values()) ==
        sum(s.allocs for s in report.sites if s.sizes),
        "y sumado en el arbol da lo mismo que sitio a sitio")
    say(tree.allocs >= sum(total.values()),
        "sin pasarse del total del arbol")


def check_old_export_still_loads(tmp, say):
    """An export written before these columns existed is not an error."""
    report = load(os.path.join(tmp, "old"), mask=False, module=False,
                  site_sizes=False, foreign=False)
    say(len(report.sites) == len(fixtures.SITES),
        "un volcado viejo (sin class_mask ni module) carga igual")
    # Y aqui la ausencia NO se lee como "no se": cuando ese volcado se
    # escribio, nada mas que el programa podia reservar, asi que ninguno de
    # sus sitios ERA de otro modulo.  Contestar "no se" pondria el filtro de
    # la pagina a apartar cosas que si eran nuestras.
    say(all(not s.foreign for s in report.sites),
        "y sin columna `foreign`, ninguno es de otro modulo -- que es lo que "
        "ese volcado queria decir, no un 'no se'")
    say(all(s.mask == 0 for s in report.sites),
        "sin mascara, cero -- que es 'no se', no un dato inventado")
    frames = report.frames[0]
    say(all(f.module == "" for f in frames),
        "y sin modulo, vacio")
    say(all(not s.sizes for s in report.sites),
        "y sin site_sizes.csv, sin reparto -- que no es un error, es un "
        "volcado que sabia menos")


def check_warnings_are_keys(report, say):
    keys = [k for k, _ in report.warnings()]
    say("warn.evicted" in keys,
        "avisa de que hubo desalojos (las cuentas son cotas inferiores)")
    say("warn.unresolved" in keys, "y de los sitios sin resolver")
    say(all(isinstance(k, str) and k.startswith("warn.") for k in keys),
        "los avisos viajan como CLAVE, no como frase ya traducida")


def check_catalogue(say):
    """A half-translated page reads like a finished one.  Not on our watch."""
    base = set(STRINGS["en"])
    for code, _ in LANGS:
        missing = base - set(STRINGS[code])
        extra = set(STRINGS[code]) - base
        say(not missing, "'%s' no deja ninguna clave sin traducir "
                         "(faltan %d)" % (code, len(missing)))
        say(not extra, "'%s' no tiene claves que el ingles no tenga" % code)
    say(t("es", "no.existe") == "[no.existe]",
        "una clave que no existe SALE como clave, no vacia")
    say(t("es", "det.nsites", n=3) == "3 sitios",
        "y los parametros se rellenan")


def run(tmp, say):
    report = load(tmp)
    check_loads_everything(report, say)
    check_nothing_is_lost(report, say)
    check_functions_group(report, say)
    check_purposes_travel(report, say)
    check_sizes_per_site(report, say)
    check_old_export_still_loads(tmp, say)
    check_warnings_are_keys(report, say)
    check_catalogue(say)
    return report

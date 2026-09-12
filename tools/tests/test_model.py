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
from alloc_tree.tree import build_tree, group_frame  # noqa: E402

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

    # Y LOS BYTES, que son la otra mitad y no salen de la primera: la casilla
    # con MENOS reservas de todo el fixture es la que mas bytes pidio, que es
    # justo el caso que una vista por cuentas esconde.  Si esto se pudiera
    # deducir de las cuentas, la columna sobraria.
    weighed = {}
    for site in report.sites:
        for b, n in site.size_bytes.items():
            weighed[b] = weighed.get(b, 0) + n
    say(bool(weighed), "y cada sitio trae tambien los bytes de cada casilla")
    top_by_allocs = max(total, key=lambda b: total[b])
    top_by_bytes = max(weighed, key=lambda b: weighed[b])
    say(top_by_allocs != top_by_bytes,
        "y la casilla que mas reservas tiene NO es la que mas bytes pidio -- "
        "que es la razon entera de contar las dos cosas")


def check_sizes_without_bytes(tmp, say):
    """Un volcado anterior a la columna de bytes: ausencia, no ceros."""
    report = load(os.path.join(tmp, "nobytes"), size_bytes=False)
    say(all(s.sizes for s in report.sites if s.sid in (0, 1, 2, 3, 4)),
        "sin la columna de bytes, el reparto por cuentas sigue entero")
    say(all(not s.size_bytes for s in report.sites),
        "y los bytes por casilla no estan -- vacio es 'no se midio', que no "
        "es lo mismo que cero")


def check_live_sizes(tmp, report, say):
    """Lo que cada tamano SE QUEDA, y cuando eso no se puede saber."""
    sizes = report.check.sizes
    say(len(sizes) == len(fixtures.CHECK_SIZES),
        "el comprobador trae una fila por casilla con lo que se queda")
    last = sizes[-1]
    say(last["allocs"] < sizes[0]["allocs"] and last["live"] > sizes[0]["live"],
        "y la casilla con menos reservas es la que mas deja vivo -- que es lo "
        "que la cuenta no puede enseñar")
    say(all(r["live"] is not None for r in sizes),
        "con el eje, lo vivo consta en todas")

    # SIN EJE no se sabe: un bloque grande nace y no muere nunca, asi que lo
    # vivo de su casilla seria todo lo que llego a servir.  Nulo y no cero.
    without = load(os.path.join(tmp, "noaxis"), live_sizes=False)
    say(all(r["live"] is None for r in without.check.sizes),
        "y sin el, lo vivo es 'no consta' -- nulo, no cero, que se leeria "
        "como 'este tamano no se queda nada'")
    say(all(r["bytes"] > 0 for r in without.check.sizes),
        "mientras que lo entregado se cuenta igual en los dos casos")


def check_grouping_hangs_on_the_author(report, say):
    """De QUE marco cuelga una reserva al agrupar por modulo o por fichero.

    EL CASO ES LA PILA ENTERA DEL COMPROBADOR: el aparato de medida por dentro,
    la biblioteca estandar en medio, UN marco del autor, y como el sistema entro
    en el hilo por fuera.  Con la regla de "el marco de mas afuera" esa pila
    caia bajo `ntdll.dll`; con la del autor cae donde se escribio.

    No es una preferencia.  Medido en una compilacion de 144.000 lineas con
    ocho marcos recorridos, la regla vieja mandaba 7.536 de 8.611 sitios a
    `KERNEL32.DLL`, `ntdll.dll` o el arranque del CRT: agrupar por modulo
    contestaba y no decia nada.
    """
    chain = report.check.chains.get(9) or []
    site = [s for s in report.check.sites if s.sid == 9][0]
    say(bool(chain) and chain[-1].module == "ntdll.dll",
        "la pila de prueba acaba, por fuera, en como el sistema entro al hilo")
    say(group_frame(site, chain, "module").function == "vm.exe",
        "y aun asi se agrupa por el modulo del marco del AUTOR, no por el del "
        "arranque del hilo")
    say(group_frame(site, chain, "file").function.endswith("emit.cpp"),
        "y por fichero, igual: el que escribio alguien")


def check_region_measured(tmp, report, say):
    """Lo que cuestan los rangos del asignador, corte a corte."""
    cuts = report.check.epochs
    say(all("region" in c for c in cuts),
        "cada corte trae lo que costaban los rangos del asignador")
    # El primer corte se toma antes de que haya rango: cero es "no se pudo
    # preguntar".  Leerlo como una medida diria que el asignador no tenia nada.
    say(cuts[0]["region"] == 0,
        "y el primero, antes de que hubiera rango, va a cero")
    later = cuts[2]
    say(later["live"] < later["region"] < later["committed"],
        "y despues queda entre lo vivo y lo comprometido -- la holgura por "
        "debajo, lo que no es de este asignador por encima")

    # Y EL HUECO, REPARTIDO EN SUS DOS MITADES.  Son la razon entera de la
    # columna: lo que esta en listas de libres podria volver al sistema y lo
    # que no, no.  Medido en una corrida de verdad el reparto era 197 MiB
    # contra 664, asi que confundirlas es atacar la cuarta parte del problema.
    say(all(c["free_spans"] + c["empty_chunks"] <= c["region"] - c["live"]
            for c in cuts if c["region"]),
        "lo recuperable es una PARTE del hueco, nunca mas que el")
    last = cuts[-1]
    say(last["free_spans"] > 0 and last["empty_chunks"] > 0
        and last["region"] - last["live"] - last["free_spans"]
            - last["empty_chunks"] > 0,
        "y el hueco tiene sus TRES partes: tramos libres, trozos con todo "
        "muerto, y trozos con algo vivo repartido")
    # LAS DOS PRIMERAS SON DEL ASIGNADOR Y LA TERCERA NO, que es la razon de
    # medirlas por separado: en una corrida de verdad eran 211 + 386 contra
    # 279 MiB, y darlas juntas decidiria mal de quien es el arreglo.
    say(last["empty_chunks"] > last["free_spans"],
        "y la mayor de las recuperables son los trozos vacios, no los tramos")

    # Una tabla escrita antes de que existiera la columna no es un error: se
    # lee como "no se midio", que es lo que la curva necesita para no salir.
    old = os.path.join(tmp, "sinregion", "export")
    fixtures.write_export(old)
    path = os.path.join(old, "check_epochs.csv")
    with open(path, encoding="utf-8") as f:
        lines = f.read().splitlines()
    # Se quitan LAS DOS columnas nuevas, que es lo que tenia un volcado de
    # antes de medirlas: quitar solo una probaria un formato que no existio.
    kept = [",".join(c for i, c in enumerate(ln.split(",")) if i not in (5, 6))
            for ln in lines]
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(kept) + "\n")
    without = Report(old)
    say(all(c["region"] == 0 and c["free_spans"] == 0
            for c in without.check.epochs),
        "y un volcado sin esas columnas carga igual, con las dos sin medir")


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
    check_sizes_without_bytes(tmp, say)
    check_live_sizes(tmp, report, say)
    check_grouping_hangs_on_the_author(report, say)
    check_region_measured(tmp, report, say)
    check_old_export_still_loads(tmp, say)
    check_warnings_are_keys(report, say)
    check_catalogue(say)
    return report

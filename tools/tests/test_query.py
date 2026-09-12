# -*- coding: utf-8 -*-
#
# VestaVM -- Distributed Virtual Machine
#
# Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
# License: MIT (see LICENSE).  Part of the VestaVM family.
"""Que preguntarle a las tablas conteste sobre las tablas.

LO QUE SE COMPRUEBA AQUI es justo lo que un guion escrito a mano se equivoca
en silencio, que es la razon de que el modo consulta exista:

- una celda VACIA no es un cero -- ni al sumar, ni al ordenar, ni al comparar;
- los numeros se comparan y se ordenan como numeros, no como texto;
- los marcos que se pegan a una fila salen de la poblacion QUE TOCA: las
  tablas del asignador cruzan contra `frames.csv` y las del comprobador contra
  `check_frames.csv`, y en el fixture los identificadores de las dos no se
  solapan a proposito -- cruzarlas daria vacio, no un nombre equivocado, y eso
  es lo que hace que la prueba lo pille.

Cada fallo de estos da una tabla que PARECE bien.  Ninguno da una excepcion.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))          # tools/

import fixtures                                     # noqa: E402
from alloc_tree import query                        # noqa: E402
from alloc_tree.report import Report                # noqa: E402


def load(tmp):
    directory = os.path.join(tmp, "export")
    fixtures.write_export(directory)
    return Report(directory)


def load_unmeasured(tmp):
    """La misma exportacion pero SIN lo vivo por tamano.

    Hace falta una de verdad y no un apano: `check_sizes.csv` deja esas dos
    columnas VACIAS cuando la corrida no pudo saberlo, y esas celdas son el
    unico sitio del fixture donde se puede probar que un hueco no se convierte
    en un cero.  Con la exportacion completa todas vienen llenas y las tres
    comprobaciones pasarian sin comprobar nada.
    """
    directory = os.path.join(tmp, "sinvivos")
    fixtures.write_export(directory, live_sizes=False)
    return Report(directory)


def column(result, name):
    """Los valores de una columna del resultado, en orden."""
    index = result.titles.index(name)
    return [row[index] for row in result.rows]


def check_typing(say):
    """Una celda es lo que es, y lo vacio es NULO."""
    say(query.typed("42") == 42 and isinstance(query.typed("42"), int),
        "un entero se lee como entero")
    say(query.typed("1.5") == 1.5,
        "y un decimal como decimal")
    say(query.typed("main") == "main",
        "y lo que no es un numero se queda como texto")
    say(query.typed("") is None and query.typed("  ") is None,
        "una celda VACIA es nulo -- no cero, que es lo unico que se puede "
        "confundir con una medida")


def check_names(report, say):
    """Abreviar el nombre de una tabla, y que una abreviatura ambigua FALLE."""
    say(query.resolve_table(report, "sites.csv") == "sites.csv",
        "el nombre entero vale")
    say(query.resolve_table(report, "epochs") == "check_epochs.csv",
        "y `epochs` llega a `check_epochs.csv` sin escribirlo entero")
    # `sizes` casa con `sizes.csv` de forma EXACTA, asi que no es ambiguo
    # aunque haya `check_sizes.csv` y `site_sizes.csv`: lo exacto gana.
    say(query.resolve_table(report, "sizes") == "sizes.csv",
        "y una coincidencia exacta gana a las que solo contienen el nombre")
    ambiguous = False
    try:
        query.resolve_table(report, "site")
    except ValueError as exc:
        ambiguous = "site_sizes.csv" in str(exc)
    say(ambiguous,
        "una abreviatura que casa con varias es un ERROR que las nombra -- "
        "elegir una contestaria sobre la tabla que no se pregunto")
    unknown = False
    try:
        query.resolve_table(report, "nada")
    except ValueError as exc:
        unknown = "sites.csv" in str(exc)
    say(unknown, "y una que no existe dice cuales hay")


def check_join(report, say):
    """Los marcos pegados, y de la poblacion correcta."""
    result = query.select(report, "sites.csv",
                          cols="site_id,func,inner,module,where")
    funcs = dict(zip(column(result, "site_id"), column(result, "func")))
    say(funcs.get(0) == "vx_parse_file",
        "un sitio se cuelga del primer marco que escribio el AUTOR, saliendo "
        "de la libreria hacia fuera")
    inner = dict(zip(column(result, "site_id"), column(result, "inner")))
    say(inner.get(0) == fixtures.NASTY,
        "y la de dentro aparte, que contesta otra cosa")
    modules = dict(zip(column(result, "site_id"), column(result, "module")))
    say(modules.get(0) == "vx",
        "el modulo es el del marco de fuera, igual que al agrupar el arbol")
    say(funcs.get(4) == "",
        "un sitio sin marcos da vacio, no revienta -- que es lo que hace el "
        "resolutor cuando no supo nada")

    # LA PRUEBA QUE IMPORTA: las dos poblaciones no se cruzan.  En el fixture
    # los sitios del comprobador son 7/9/11 y los del asignador 0..6, asi que
    # cruzarlos daria vacio en las dos direcciones.
    check = query.select(report, "check_sites.csv",
                         cols="stack_id,func,inner,outer")
    names = dict(zip(column(check, "stack_id"), column(check, "func")))
    say(names.get(7) == "Parser::take",
        "una pila del COMPROBADOR se cruza contra sus propios marcos")
    say(all(v for v in names.values()),
        "y ninguna se queda sin nombre, que es lo que pasaria al cruzarla "
        "contra la poblacion del asignador")

    # LA PILA ENTERA, que es donde las dos reglas se separan.  La 9 va desde el
    # aparato de medida por dentro hasta como el sistema entro en el hilo por
    # fuera, con la libreria estandar y UN marco del autor en medio.
    #
    # Esto no es una preferencia: con el marco de mas afuera, subir la
    # profundidad del recorrido de 4 a 16 en una corrida real movia el pico de
    # `vx::Lowering::emit` a `RtlUserThreadStart` -- cuanto mas lejos se camina,
    # mas se parece ese marco al arranque del hilo, que es el mismo para todo el
    # programa y no distingue nada.  Con MAS datos, peor respuesta.
    row = dict(zip(column(check, "stack_id"), column(check, "outer")))
    say(names.get(9) == "Emitter::grow",
        "y de una pila entera se coge el marco del AUTOR, ni el aparato de "
        "medida por dentro ni la libreria que hay en medio")
    say(row.get(9) == "RtlUserThreadStart",
        "mientras que el de mas afuera -- otra pregunta, y a mano -- es como "
        "el sistema entro en el hilo")
    inner = dict(zip(column(check, "stack_id"), column(check, "inner")))
    say(inner.get(9) == "util::detail::current_cache()",
        "y el de mas adentro es el aparato de medida, que es lo que de verdad "
        "hizo la reserva")


def check_pairs(report, say):
    """La tabla de parejas: dos sitios en una fila, y sin pisarse."""
    columns = query.pseudo_columns(report, "check_pairs.csv")
    say("alloc_stack" not in columns and "free_stack" not in columns,
        "las columnas pegadas NO tapan a `alloc_stack` ni a `free_stack`, que "
        "son los identificadores")
    say("alloc_chain" in columns and "free_func" in columns,
        "la cadena de llamadas de cada lado se llama `chain`, que es un "
        "nombre libre")
    result = query.select(report, "check_pairs.csv",
                          cols="alloc_stack,alloc_func,free_func")
    say(column(result, "alloc_stack")[0] == 7,
        "y `alloc_stack` sigue siendo el numero que era")
    say(column(result, "alloc_func")[0] == "Parser::take",
        "con el nombre de quien entrego al lado")


def check_where(report, blank, say):
    """La condicion, y las filas que NO se pueden juzgar."""
    result = query.select(report, "sites.csv", where="bytes > 5000",
                          cols="site_id,bytes")
    say(sorted(column(result, "site_id")) == [0, 2],
        "los numeros se comparan como numeros -- por texto, 640 pasaria de "
        "40000")
    say(result.skipped == 5 and result.unsure == 0,
        "y se dice cuantas filas no cumplian")

    result = query.select(report, "sites.csv", where='module == "vx"',
                          cols="site_id")
    say(sorted(column(result, "site_id")) == [0, 1, 6],
        "se puede filtrar por una columna PEGADA, que es la mitad de la "
        "gracia de pegarlas")

    result = query.select(report, "sites.csv", where='has(func, "PARSE")',
                          cols="site_id")
    say(sorted(column(result, "site_id")) == [0, 1],
        "y buscar dentro de un texto sin pelearse con las mayusculas")

    # Una columna que viene vacia en algunas filas: comparar eso con un numero
    # no es "no cumple", es "no se sabe", y se cuenta aparte.
    result = query.select(blank, "check_sizes.csv", where="live_bytes > 0",
                          cols="bucket,live_bytes")
    say(result.unsure > 0,
        "una celda sin medir contra un numero NO cuenta como 'no cumple': se "
        "cuenta como no juzgada, y se dice")
    say(all(v is not None for v in column(result, "live_bytes")),
        "y ninguna fila sin medir se cuela como si cumpliera")


def check_derived(report, blank, say):
    """Columnas calculadas, con nombre, y el nulo que sobrevive a la cuenta."""
    result = query.select(report, "sites.csv",
                          cols="site_id,kib(bytes) as kib,pct(over_bytes,"
                               "bytes) as sobra")
    say("kib" in result.titles and "sobra" in result.titles,
        "una columna calculada se llama como se le diga")
    kib = dict(zip(column(result, "site_id"), column(result, "kib")))
    say(abs(kib[2] - 40000 / 1024.0) < 0.01,
        "y vale lo que tiene que valer")
    result = query.select(blank, "check_sizes.csv",
                          cols="bucket,mib(live_bytes) as vivo")
    say(any(v is None for v in column(result, "vivo")),
        "lo que no se midio SIGUE sin medirse despues de pasarlo a MiB -- no "
        "se convierte en un cero en el ultimo paso")
    say(result.nulls.get("vivo", 0) > 0,
        "y se cuenta cuantas veces, porque en una columna de numeros un hueco "
        "es la trampa")

    # En una columna de TEXTO, en cambio, no se avisa: un hueco es un hueco y
    # nadie lo confunde con un cero.  Un aviso que sale siempre no se lee.
    result = query.select(report, "check_epochs.csv", cols="epoch,mark")
    say("mark" not in result.nulls,
        "de una columna de texto vacia no se avisa: ahi un hueco no se puede "
        "leer como una medida")


def check_sorting(report, blank, say):
    """Ordenar por una columna de SALIDA, y los nulos al final."""
    result = query.select(report, "sites.csv", cols="site_id,bytes")
    query.sort_rows(result, "-bytes")
    say(column(result, "bytes")[0] == 40000,
        "de mayor a menor, por valor y no por texto")
    query.sort_rows(result, "bytes")
    say(column(result, "bytes")[0] == 448,
        "y al reves")

    result = query.select(blank, "check_sizes.csv",
                          cols="bucket,live_bytes")
    query.sort_rows(result, "-live_bytes")
    values = column(result, "live_bytes")
    first_null = next((i for i, v in enumerate(values) if v is None),
                      len(values))
    say(all(v is None for v in values[first_null:]),
        "los nulos van al FINAL: encabezar una tabla de mayor a menor con las "
        "filas de las que no se sabe nada es lo contrario de lo que se pidio")

    bad = False
    try:
        query.sort_rows(result, "noexiste")
    except ValueError as exc:
        bad = "live_bytes" in str(exc)
    say(bad, "y ordenar por una columna que no esta dice cuales hay")


def check_grouping(report, say):
    """Agrupar suma lo numerico y dice lo que no pudo sumar."""
    result = query.select(report, "sites.csv", group_by="module",
                          cols="allocs,bytes")
    rows = dict(zip(column(result, "module"), column(result, "bytes")))
    say(rows.get("vx") == 6400 + 3200 + 768,
        "los bytes de un modulo son la suma de sus sitios")
    counts = dict(zip(column(result, "module"), column(result, "rows")))
    say(counts.get("vx") == 3,
        "y viene con cuantas filas se sumaron, que es lo que permite ver que "
        "un total sale de una sola")

    result = query.select(report, "sites.csv", group_by="module",
                          cols="tag_name,bytes")
    say("tag_name" in result.unsummable,
        "una columna de texto agrupada sale en blanco Y SE DICE -- si no, se "
        "leeria como que ahi no habia nada")


def check_grep(report, say):
    """Buscar en todas las tablas a la vez."""
    hits = query.grep(report, "vx_parse_file")
    say(any(name == "frames.csv" for name, _, _, _ in hits),
        "encuentra un simbolo y dice en que TABLA estaba")
    say(all(len(hit) == 4 for hit in hits),
        "y trae la fila entera, no solo la celda")
    say(query.grep(report, "no-existe-esto-en-ningun-sitio") == [],
        "y no encontrar nada es una lista vacia, no un error")


def run(tmp, say):
    report = load(tmp)
    blank = load_unmeasured(tmp)
    check_typing(say)
    check_names(report, say)
    check_join(report, say)
    check_pairs(report, say)
    check_where(report, blank, say)
    check_derived(report, blank, say)
    check_sorting(report, blank, say)
    check_grouping(report, say)
    check_grep(report, say)
    return report

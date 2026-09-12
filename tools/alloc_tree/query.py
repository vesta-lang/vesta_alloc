# -*- coding: utf-8 -*-
#
# VestaVM -- Distributed Virtual Machine
#
# Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
# License: MIT (see LICENSE).  Part of the VestaVM family.
"""Preguntarle cosas a las tablas, sin escribir un guion cada vez.

POR QUE EXISTE.  La exportacion son catorce ficheros que se cruzan por
`site_id` / `stack_id`, y la mitad de las preguntas que se le hacen a una
medicion no son "ensename el arbol" sino una consulta: los cortes donde la
holgura fue peor, los sitios de un modulo ordenados por bytes, en que tabla
aparece un simbolo.  Eso se acababa contestando con un `python -c` distinto
cada vez -- y un guion escrito a mano no se revisa, no se guarda y se equivoca
en silencio: una columna vacia sumada como cero, un `site_id` cruzado contra la
poblacion que no era, un orden por texto sobre numeros que deja el 9 por encima
del 100.

Aqui esas tres trampas estan resueltas UNA vez:

- **Una celda vacia es NULO, no cero.**  Las tablas lo usan a proposito: en
  `check_sizes.csv` lo vivo va vacio cuando la corrida no pudo saberlo, y un
  cero ahi se leeria como "este tamano no se queda nada".  Un nulo no suma, no
  ordena y no casa; y **se CUENTA**, para que el informe diga cuantas filas no
  pudo juzgar en vez de dejarlas fuera sin avisar.
- **Los numeros se comparan como numeros.**  La celda llega como texto y se
  convierte antes de nada.
- **Los marcos se pegan solos**, y de la poblacion correcta: las tablas del
  asignador cruzan contra `frames.csv` y las del comprobador contra
  `check_frames.csv`.  Son dos poblaciones distintas -- una direccion de
  retorno contra una pila recorrida -- y mezclarlas contestaria otra pregunta.

QUE NO HACE.  No calcula nada que no este medido.  Una columna derivada es una
cuenta sobre lo que hay (restar dos columnas, pasarlo a MiB); de aqui no sale
ningun numero que no salga de la exportacion.
"""
import re

from . import ours

# Las tablas que llevan un sitio, con la columna que lo nombra y de que
# POBLACION son sus marcos.  Sin esto habria que elegir a mano, y elegir mal no
# da un error: da el nombre de otra funcion.
SITE_TABLES = {
    "sites.csv": ("site_id", "report"),
    "site_sizes.csv": ("site_id", "report"),
    "check_sites.csv": ("stack_id", "check"),
    "check_epoch_sites.csv": ("stack_id", "check"),
    "check_peak.csv": ("stack_id", "check"),
}

# `check_pairs.csv` lleva DOS sitios -- quien entrego y quien devolvio --, asi
# que sus columnas pegadas van con prefijo.  Es la unica tabla asi, y por eso
# va aparte en vez de complicar la de arriba.
PAIR_TABLE = "check_pairs.csv"
PAIR_KEYS = (("alloc", "alloc_stack"), ("free", "free_stack"))

# Lo que se le pega a una fila con sitio.  El nombre dice desde donde se mira:
# `func` es la funcion que EXISTE en el binario (la de fuera), que es la que
# contesta "quien lo pidio"; `inner` es la de dentro, que contesta
# "std::string" -- cierto e inutil para decidir nada.
#
# La cadena se llama `chain` y no `stack` POR UNA COLISION: `check_pairs.csv`
# ya tiene columnas `alloc_stack` y `free_stack`, que son los IDENTIFICADORES
# de los dos sitios.  Con el otro nombre, la columna pegada tapaba a la de
# verdad y `alloc_stack` pasaba de ser un numero a ser una cadena de llamadas
# sin que nada lo dijera.
SITE_FIELDS = ("func", "inner", "outer", "file", "where", "module", "chain")


def known_tables(report):
    """Las tablas cargadas, en el orden en que se leyeron."""
    return [name for name in report.raw if report.raw[name] or
            report.columns.get(name)]


def resolve_table(report, name):
    """El nombre completo de una tabla a partir de como se escribio.

    Se admite `epochs` por `check_epochs.csv`: escribir el nombre entero cada
    vez es lo que hace que una herramienta no se use.  Una abreviatura que case
    con dos tablas es un ERROR que las nombra -- elegir una seria contestar
    sobre la que no se pregunto.
    """
    names = known_tables(report)
    if name in names:
        return name
    for candidate in (name + ".csv", "check_" + name + ".csv",
                      "check_" + name):
        if candidate in names:
            return candidate
    hits = [n for n in names if name in n]
    if len(hits) == 1:
        return hits[0]
    if hits:
        raise ValueError("`%s` casa con varias tablas: %s"
                         % (name, ", ".join(hits)))
    raise ValueError("no hay ninguna tabla `%s`.  Hay: %s"
                     % (name, ", ".join(names)))


def typed(text):
    """La celda como lo que es: entero, decimal, texto -- o NULO si esta vacia.

    El nulo es lo que importa.  Convertir una celda vacia a cero es la forma
    mas facil de que una tabla mienta: `live_bytes` vacio quiere decir que la
    corrida no lo pudo medir, y sumarlo como cero da un total que parece una
    medida.
    """
    if text is None:
        return None
    text = text.strip()
    if text == "":
        return None
    try:
        return int(text)
    except ValueError:
        pass
    try:
        return float(text)
    except ValueError:
        return text


def chain_fields(chain):
    """Lo que se le pega a una fila con sitio, ya calculado."""
    if not chain:
        return dict.fromkeys(SITE_FIELDS, "")
    # `ours.owning_frame` y no una copia: la pagina, el arbol de la terminal y
    # esto tienen que nombrar igual al mismo sitio.
    owner = ours.owning_frame(chain)
    return {
        "func": owner.function,
        "inner": chain[0].function,
        # El de mas afuera, que es OTRA pregunta -- por donde se entro a esto --
        # y sigue a mano por si se quiere.
        "outer": chain[-1].function,
        "file": owner.file,
        "where": owner.where(),
        "module": owner.module,
        # De dentro afuera, que es como se lee una cadena de llamadas.
        "chain": " < ".join(f.function for f in chain),
    }


class Joiner(object):
    """Los marcos de cada sitio, calculados una vez y guardados.

    Una consulta mira la misma cadena tantas veces como columnas pegadas pida
    y tantas como filas tenga el sitio; recorrerla cada vez convierte una
    consulta sobre `check_epoch_sites.csv` -- decenas de miles de filas -- en
    algo que se nota.
    """

    def __init__(self, report):
        self.report = report
        self._cache = {}

    def fields(self, population, key):
        """Las columnas pegadas del sitio `key` en esa poblacion."""
        hit = self._cache.get((population, key))
        if hit is not None:
            return hit
        chain = []
        if key is not None:
            if population == "check":
                check = getattr(self.report, "check", None)
                if check is not None:
                    chain = check.chains.get(key) or []
            else:
                chain = self.report.frames.get(key) or []
        fields = chain_fields(chain)
        self._cache[(population, key)] = fields
        return fields


def pseudo_columns(report, table):
    """Las columnas que esta tabla gana al pegarle sus marcos."""
    real = report.columns.get(table, [])
    if table == PAIR_TABLE:
        names = ["%s_%s" % (prefix, field)
                 for prefix, _ in PAIR_KEYS
                 for field in SITE_FIELDS]
        return tuple(n for n in names if n not in real)
    if table in SITE_TABLES:
        # Una columna de VERDAD manda siempre: `check_frames.csv` ya trae
        # `module`, y taparla con la calculada seria enseñar otra medida con el
        # mismo nombre.
        return tuple(f for f in SITE_FIELDS if f not in real)
    return ()


def namespace_of(report, joiner, table, row):
    """El espacio de nombres con el que se evalua una expresion sobre la fila.

    Las celdas de verdad primero y las pegadas despues, pero sin pisar:
    `pseudo_columns` ya quito las que chocan, y esto lo respeta.
    """
    space = {}
    for key, value in row.items():
        if key is not None:
            space[key] = typed(value)
    if table == PAIR_TABLE:
        for prefix, column in PAIR_KEYS:
            fields = joiner.fields("check", typed(row.get(column)))
            for name, value in fields.items():
                # `setdefault`, igual que abajo: una columna de VERDAD manda
                # siempre sobre una pegada que se llame igual.
                space.setdefault("%s_%s" % (prefix, name), value)
    elif table in SITE_TABLES:
        column, population = SITE_TABLES[table]
        fields = joiner.fields(population, typed(row.get(column)))
        for name, value in fields.items():
            space.setdefault(name, value)
    return space


# --------------------------------------------------------------------------
#  Las funciones que se pueden usar dentro de una expresion
# --------------------------------------------------------------------------
#
# Pocas y a proposito: lo que hace falta para leer una tabla de memoria, no un
# lenguaje.  Todas devuelven NULO cuando les entra un nulo -- que es lo que
# hace que una columna sin medir siga sin medir despues de pasarla a MiB, en
# vez de convertirse en un cero que parece una medida.


def _scaled(value, unit):
    if value is None:
        return None
    return round(float(value) / unit, 2)


def fn_kib(value):
    """El valor en KiB."""
    return _scaled(value, 1024.0)


def fn_mib(value):
    """El valor en MiB."""
    return _scaled(value, 1048576.0)


def fn_gib(value):
    """El valor en GiB."""
    return _scaled(value, 1073741824.0)


def fn_pct(part, whole):
    """Que tanto por ciento es `part` de `whole`."""
    if part is None or not whole:
        return None
    return round(100.0 * float(part) / float(whole), 2)


def fn_like(text, pattern):
    """Si el texto casa con la expresion regular, sin distinguir mayusculas."""
    if text is None:
        return False
    return re.search(pattern, str(text), re.IGNORECASE) is not None


def fn_has(text, needle):
    """Si el texto CONTIENE eso, sin distinguir mayusculas."""
    if text is None:
        return False
    return str(needle).lower() in str(text).lower()


HELPERS = {
    "kib": fn_kib, "mib": fn_mib, "gib": fn_gib, "pct": fn_pct,
    "like": fn_like, "has": fn_has,
    "abs": abs, "min": min, "max": max, "len": len, "round": round,
    "int": int, "float": float, "str": str, "bool": bool,
}


def evaluate(code, space):
    """Una expresion ya compilada sobre una fila.

    Sin `__builtins__`: esto lee una carpeta de medidas, no ejecuta un
    programa, y dejar entrar el resto del lenguaje no anade ni una pregunta que
    se pueda contestar.
    """
    return eval(code, {"__builtins__": {}}, dict(HELPERS, **space))


def split_top(spec):
    """Parte por comas que NO esten dentro de parentesis ni de comillas.

    Un `--cols mib(a-b) as hueco, func` tiene una coma dentro de ninguna, pero
    `pct(bytes, total)` si -- y partir por ahi daria dos expresiones rotas con
    un mensaje que no apunta a la coma.
    """
    parts = []
    depth = 0
    quote = ""
    current = []
    for char in spec:
        if quote:
            if char == quote:
                quote = ""
            current.append(char)
            continue
        if char in "'\"":
            quote = char
            current.append(char)
            continue
        if char in "([":
            depth += 1
        elif char in ")]":
            depth -= 1
        if char == "," and depth == 0:
            parts.append("".join(current))
            current = []
            continue
        current.append(char)
    parts.append("".join(current))
    return [p.strip() for p in parts if p.strip()]


class Column(object):
    """Una columna de salida: como se llama y como se calcula."""

    __slots__ = ("title", "source", "code")

    def __init__(self, spec):
        source = spec
        title = None
        # ` as <nombre>` al final.  Se busca por la derecha para que una
        # expresion que contenga la palabra no se parta por en medio.
        match = re.search(r"\s+as\s+([A-Za-z_][A-Za-z0-9_]*)\s*$", spec)
        if match:
            title = match.group(1)
            source = spec[:match.start()]
        self.source = source.strip()
        self.title = title or self.source
        self.code = compile(self.source, "<cols>", "eval")


def columns_for(report, table, spec):
    """Las columnas pedidas, o todas las de la tabla si no se pidio ninguna."""
    if spec:
        return [Column(part) for part in split_top(spec)]
    names = list(report.columns.get(table, []))
    names.extend(pseudo_columns(report, table))
    return [Column(name) for name in names]


class Result(object):
    """Lo que salio, y lo que NO se pudo mirar.

    Las dos cosas juntas y no solo la primera: una consulta que descarta filas
    sin decirlo da una tabla que parece completa, y el modo de fallo de eso es
    justo el que esta herramienta existe para no tener.
    """

    __slots__ = ("titles", "rows", "total", "skipped", "unsure", "nulls",
                 "unsummable", "_textual")

    def __init__(self, titles):
        self.titles = titles
        self.rows = []
        self.total = 0     # filas que casaron, antes de cortar por `rows`
        self.skipped = 0   # filas que la condicion dejo fuera
        self.unsure = 0    # filas que la condicion no pudo juzgar
        self.nulls = {}    # columna -> cuantas celdas venian sin medir
        self.unsummable = []  # columnas que al agrupar no se pudieron sumar
        self._textual = set()  # columnas en las que salio algo que no es numero

    def saw(self, title, value):
        """Apunta lo que dio una celda: si no vino nada, y si no era un numero."""
        if value is None:
            self.nulls[title] = self.nulls.get(title, 0) + 1
        elif number_of(value) is None:
            self._textual.add(title)

    def settle(self):
        """Deja en `nulls` solo lo que podria leerse como un cero.

        Se descarta la columna en la que salio TEXTO -- la fase de un corte,
        que casi siempre esta vacia --: ahi un hueco es un hueco y nadie lo
        confunde con una medida, y avisar en cada consulta seria un aviso que
        se aprende a no leer, que es peor que no darlo.

        Y se AVISA de la columna que no dio nada en ninguna fila, que es el
        caso contrario y el que mas engana: una columna de numeros entera en
        blanco no es un cero repetido, es una corrida que no pudo medir eso.
        """
        self.nulls = {k: v for k, v in self.nulls.items()
                      if k not in self._textual}
        return self


def number_of(value):
    """El valor si es un numero; si no, nulo.  Para sumar y para ordenar."""
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        return value
    return None


def select(report, table, where=None, cols=None, group_by=None):
    """Las filas de una tabla que cumplen la condicion, con sus columnas.

    @param where   expresion sobre la fila, o nada para todas.
    @param cols    columnas de salida separadas por comas, con `expr as nombre`.
    @param group_by expresion por la que agrupar; con ella, cada columna
                   numerica se SUMA y las demas se dejan fuera diciendolo.
    """
    joiner = Joiner(report)
    outputs = columns_for(report, table, cols)
    condition = compile(where, "<where>", "eval") if where else None
    group = Column(group_by) if group_by else None

    result = Result([c.title for c in outputs])
    if group is not None:
        return _grouped(report, table, joiner, outputs, condition, group,
                        result)

    for row in report.raw.get(table, []):
        space = namespace_of(report, joiner, table, row)
        if condition is not None:
            try:
                if not evaluate(condition, space):
                    result.skipped += 1
                    continue
            except TypeError:
                # Casi siempre una celda sin medir contra un numero.  NO se
                # cuenta como "no cumple": no se sabe si cumple.
                result.unsure += 1
                continue
        values = []
        for column in outputs:
            value = evaluate(column.code, space)
            result.saw(column.title, value)
            values.append(value)
        result.rows.append(values)
        result.total += 1
    return result.settle()


def _grouped(report, table, joiner, outputs, condition, group, result):
    """La parte de `select` que agrupa.  Suma lo numerico y dice lo que no."""
    keys = []
    sums = {}
    counts = {}
    for row in report.raw.get(table, []):
        space = namespace_of(report, joiner, table, row)
        if condition is not None:
            try:
                if not evaluate(condition, space):
                    result.skipped += 1
                    continue
            except TypeError:
                result.unsure += 1
                continue
        key = evaluate(group.code, space)
        if key not in sums:
            keys.append(key)
            sums[key] = [None] * len(outputs)
            counts[key] = 0
        counts[key] += 1
        for i, column in enumerate(outputs):
            raw_value = evaluate(column.code, space)
            result.saw(column.title, raw_value)
            value = number_of(raw_value)
            if value is None:
                continue    # lo que no es un numero no se suma
            if sums[key][i] is None:
                sums[key][i] = value
            else:
                sums[key][i] += value

    result.titles = [group.title, "rows"] + [c.title for c in outputs]
    for key in keys:
        result.rows.append([key, counts[key]] + sums[key])
        result.total += 1
    # UNA COLUMNA DE TEXTO AGRUPADA SALE VACIA, y eso hay que decirlo UNA vez.
    # Sin el aviso, una columna en blanco se lee como "aqui no habia nada"
    # cuando lo cierto es "esto no se puede sumar"; y con un aviso por fila
    # seria un muro que nadie lee.
    result.unsummable = [c.title for c in outputs
                         if c.title in result._textual]
    return result.settle()


def sort_rows(result, spec):
    """Ordena por una columna de SALIDA, con `-` delante para descendente.

    Por la de salida y no por una de la tabla: lo que se quiere ordenar es lo
    que se esta mirando, y muchas veces eso es una cuenta que no existe en
    ningun fichero.
    """
    descending = spec.startswith("-")
    name = spec[1:] if descending else spec
    if name not in result.titles:
        raise ValueError("no se puede ordenar por `%s`: las columnas son %s"
                         % (name, ", ".join(result.titles)))
    index = result.titles.index(name)

    def key_of(row):
        """Numeros por valor y lo demas por texto, sin mezclarlos.

        Los dos no se comparan entre si -- en Python 3 eso es un error, no un
        orden --, asi que la clave lleva delante de que clase es.
        """
        value = row[index]
        number = number_of(value)
        if number is not None:
            return (0, number, "")
        return (1, 0, str(value))

    # LOS NULOS SE APARTAN ANTES DE ORDENAR, y van al final se pida el orden
    # que se pida.  Un nulo es "no consta": encabezar una tabla de mayor a
    # menor con las filas de las que no se sabe nada es lo contrario de lo que
    # se pidio.
    known = [r for r in result.rows if r[index] is not None]
    unknown = [r for r in result.rows if r[index] is None]
    known.sort(key=key_of, reverse=descending)
    result.rows = known + unknown
    return result


def grep(report, pattern, tables=None):
    """Donde aparece eso, en TODAS las tablas.

    La pregunta de "en que fichero miro" no deberia costar catorce `grep`, y
    ademas uno por fichero no sabe en que columna cayo: aqui la respuesta trae
    la tabla, la columna y la fila entera.
    """
    rx = re.compile(pattern, re.IGNORECASE)
    hits = []
    for name in (tables or known_tables(report)):
        for index, row in enumerate(report.raw.get(name, [])):
            for column in report.columns.get(name, []):
                value = row.get(column)
                if value and rx.search(value):
                    hits.append((name, index, column, row))
                    break
    return hits

# -*- coding: utf-8 -*-
#
# VestaVM -- Distributed Virtual Machine
#
# Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
# License: MIT (see LICENSE).  Part of the VestaVM family.
"""Reading what `util::write_alloc_csv` wrote.

Five files that join on `site_id`.  Nothing is computed here: every number
comes out of the process, and this only puts the tables back together.
"""
import csv
import os


class Frame(object):
    """One step of an inlining chain."""

    __slots__ = ("function", "file", "line", "inlined", "module")

    def __init__(self, function, path, line, inlined, module=""):
        self.function = function or "(unknown)"
        self.file = path or ""
        self.line = line
        self.inlined = inlined
        # Whose code this is -- standard library, start-up, a third-party
        # library, or which module of the project's own.  It is the HOST that
        # classifies (the export carries the column): what counts as a module
        # is a property of a project's layout, and guessing at it from here
        # would be inventing an answer instead of reporting one.
        self.module = module or ""

    def where(self):
        if not self.file:
            return ""
        return "%s:%d" % (os.path.basename(self.file), self.line)


class Site(object):
    """One place that allocated, with what it did."""

    __slots__ = ("sid", "allocs", "bytes", "over", "over_bytes", "tag",
                 "classes", "large", "pc", "row", "mask", "sizes",
                 "size_bytes", "foreign")

    def __init__(self, row):
        # The row is kept WHOLE, and not only the fields used below.  A column
        # this tool does not understand is still a measurement, and dropping it
        # here would make it impossible to look for later -- including a column
        # added to the export after this file was written.
        self.row = row
        self.sid = int(row["site_id"])
        self.allocs = int(row["allocs"])
        self.bytes = int(row["bytes"])
        self.over = int(row["over_allocs"])
        self.over_bytes = int(row["over_bytes"])
        self.tag = row["tag_name"]
        self.classes = int(row["size_classes"])
        self.large = int(row["large"])
        self.pc = int(row["pc_offset"])
        # The size classes as BITS, not only as a count.  A count cannot be
        # added up across sites -- two sites using the same class would come
        # out as two -- so a tree that groups sites can only say how many
        # DISTINCT classes a branch touched if it has the bits to OR together.
        # Written by newer exports; an older one simply has no such column.
        self.mask = int(row.get("class_mask") or 0)
        # bucket -> allocations.  Filled by `Report` from `site_sizes.csv`;
        # empty when the export predates that file.
        self.sizes = {}
        # bucket -> BYTES, from the same file.  Apart from the one above and
        # not derived from it: a bucket holds a range of sizes, so a count
        # gives an interval and never a figure -- and the last bucket has no
        # ceiling, which leaves only a floor.  It is also the half that decides
        # the memory: thirty-three allocations can be an eighth of the bytes.
        self.size_bytes = {}
        # Whether the site is in a module that is NOT the program: a system
        # library, the C runtime.  It comes from the export as a FACT, because
        # only the allocator can tell -- from out here a shared library and a
        # logical module of the project look exactly the same in `module`.
        # Missing in an older export, and there "unknown" is answered as "not
        # foreign": that is what those exports meant, since nothing but the
        # program could allocate back then.
        self.foreign = (row.get("foreign") or "0").strip() == "1"


class CheckSite(object):
    """One STACK the checker saw, with what its blocks did.

    Deliberately the same shape as `Site`, so the tree builder, the text
    output and the page work on it unchanged.  The two exports answer the same
    question from different sides -- who allocated -- and giving them two
    incompatible models would mean writing every consumer twice and then
    watching the two drift.

    The names that differ are mapped rather than renamed:

        allocs  <- births        every block, released or not
        bytes   <- bytes         the same, in bytes
        tag     <- use/shape     MEASURED, not declared: the checker derives
                                 both axes from lifetime and from whether the
                                 size varied
    """

    __slots__ = ("sid", "allocs", "bytes", "over", "over_bytes", "tag",
                 "classes", "large", "pc", "row", "mask", "sizes",
                 "size_bytes", "foreign",
                 "weighed_allocs", "weighed_bytes", "deaths", "alive_blocks",
                 "alive_bytes", "life_avg", "life_max", "cross_thread",
                 "size_min", "size_max", "use", "shape", "walked", "frames",
                 "cut")

    def __init__(self, row):
        self.row = row
        self.sid = int(row["stack_id"])
        self.allocs = int(row["births"])
        self.bytes = int(row["bytes"])
        # WHAT WAS ONLY WEIGHED.  Blocks over the small-class limit are counted
        # in the bytes but not inspected, so a row's certainty is not uniform.
        # Kept apart from the totals for the same reason `over` is: folding
        # them in would produce a number that looks equally measured
        # throughout and is not.
        self.weighed_allocs = int(row.get("outside_births") or 0)
        self.weighed_bytes = int(row.get("outside_bytes") or 0)
        self.deaths = int(row.get("deaths") or 0)
        self.alive_blocks = int(row.get("alive_blocks") or 0)
        self.alive_bytes = int(row.get("alive_bytes") or 0)
        self.life_avg = int(row.get("life_avg") or 0)
        self.life_max = int(row.get("life_max") or 0)
        # Blocks released by a thread other than the one that allocated them.
        # Their lives are not comparable -- the counter belongs to a thread --
        # so the checker counts them apart instead of folding them in.
        self.cross_thread = int(row.get("cross_thread") or 0)
        self.size_min = int(row.get("size_min") or 0)
        self.size_max = int(row.get("size_max") or 0)
        self.use = row.get("use") or ""
        self.shape = row.get("shape") or ""
        # Whether the frame chain could be followed.  A single frame with this
        # false is not a shallow stack: it is a stack that could not be walked,
        # and reading it as the first would understate every branch above it.
        self.walked = (row.get("walked") or "0").strip() == "1"
        self.frames = int(row.get("frames") or 0)
        # CORTADA: habia mas que seguir y no donde ponerlo.  Distinto de una
        # pila corta, y distinto de una que no se pudo caminar: esta cuelga de
        # donde se corto, asi que el arbol la pone bajo el sitio equivocado.
        # Una exportacion anterior no traia la columna, y eso es "no consta".
        self.cut = (row.get("cut") or "0").strip() == "1"

        # --- the fields `Site` has and this export does not ----------------
        # Answered as "nothing", never invented.  `over` is the allocator
        # table's eviction inheritance; the checker's depot does not evict, it
        # fills up and says so, so there is no inherited count to report.
        self.over = 0
        self.over_bytes = 0
        self.large = 0
        self.classes = 0
        self.mask = 0
        self.sizes = {}
        self.size_bytes = {}
        self.foreign = False
        self.pc = 0
        # The purpose, MEASURED.  The allocator's `tag` is what the programmer
        # declared; this is what the blocks turned out to be.  They go in the
        # same field so that `--group purpose` and the purpose column work on
        # both exports -- and the header says which one is being shown, because
        # confusing intent with outcome is exactly the mistake worth avoiding.
        self.tag = ("%s/%s" % (self.use, self.shape)) if self.use else ""


class CheckReport(object):
    """The checker's four files, in the shape the rest of the tool expects.

    `chain_of` is where the two exports really differ.  The allocator records
    ONE return address and resolves its inlining chain; the checker walks the
    stack, so it has TWO axes -- `frame`, the call frame, and `depth`, the
    inline level inside it.  Flattening them innermost-first gives exactly what
    the tree builder already consumes, and keeps the distinction visible in the
    frame itself, so nobody has to line up two tables by eye.
    """

    def __init__(self, sites, chains, summary, pairs=None, verdicts=None,
                 epochs=None, series=None, peak=None, sizes=None,
                 size_series=None):
        self.sites = sites
        self.chains = chains          # stack_id -> [Frame], innermost first
        self.summary = summary
        # EL EJE DEL TIEMPO, vacio cuando la corrida no lo pidio -- que es lo
        # normal, porque se paga solo quien lo pide.  `epochs` son los cortes en
        # orden; `series` es stack_id -> {corte: bytes}, disperso porque un
        # sitio solo aparece en los cortes en los que tenia algo; `peak` son las
        # filas del corte mas alto, que es el unico instante que decide cuanta
        # memoria necesita la corrida.
        self.epochs = epochs or []
        self.series = series or {}
        self.peak = peak or []
        # Lo que estaba MAL, con sus pilas.  Vacio cuando no hubo ninguno, que
        # es lo normal y no un fallo de la exportacion.
        self.verdicts = verdicts or []
        # (alloc_stack, free_stack) -> {"blocks": n, "bytes": n}.  Vacio cuando
        # el fichero no esta, que es lo que pasa con una exportacion vieja: el
        # resto de la pagina no depende de esto.
        self.pairs = pairs or {}
        # QUE SE QUEDA CADA TAMANO: una fila por casilla con lo entregado, lo
        # devuelto y lo que sigue en pie.  Es la tercera lectura del histograma
        # y la unica que no sale de las otras dos -- el volcado cuenta reservas
        # y los sitios suman bytes, las dos sobre TODA la corrida, y una casilla
        # que mueve cuatro gigabytes y los devuelve todos sale ahi mas grande
        # que una que se queda con doscientos megabytes.
        self.sizes = sizes or []
        # Y LA MISMA CURVA REPARTIDA POR TAMANO: casilla -> {corte: bytes}.  Es
        # lo que permite dibujar el eje del tiempo por tamano en vez de por
        # sitio, que es la pregunta que el histograma de totales no contesta:
        # cuando se llena cada casilla, y si dos se llenan a la vez o se turnan.
        self.size_series = size_series or {}

    def owners_of(self, sid):
        """Los sitios que devolvieron los bloques de `sid`.

        Uno solo quiere decir que ese sitio tiene DUEÑO y podria irse a una
        arena propia; varios, que es compartido y su sitio es el camino comun.
        Devolver la lista y no un veredicto es deliberado: no poder demostrar
        que hay un dueño no es demostrar que hay varios, y quien decida tiene
        que ver sobre que.
        """
        return [free for (alloc, free) in self.pairs if alloc == sid]

    def chain_of(self, site):
        chain = self.chains.get(site.sid)
        if chain:
            return chain
        # A stack with no resolvable frame is still a place, and dropping it
        # would quietly shrink the totals.
        return [Frame("(stack %d)" % site.sid, "", 0, False)]

    def warnings(self):
        """What the checker could NOT do, as (key, parameters)."""
        out = []
        full = int(self.summary.get("stacks_that_did_not_fit", 0) or 0)
        if full:
            out.append(("warn.check.depotfull",
                        {"n": full,
                         "cap": int(self.summary.get("depot_slots", 0) or 0)}))
        blocks = int(self.summary.get("blocks_with_no_site", 0) or 0)
        if blocks:
            out.append(("warn.check.nosite",
                        {"n": blocks,
                         "b": int(self.summary.get("bytes_with_no_site", 0)
                                  or 0)}))
        outside = int(self.summary.get("blocks_outside_the_shadow", 0) or 0)
        if outside:
            out.append(("warn.check.outside", {"n": outside}))
        # Not a failure of the export, a property of the build: without a frame
        # pointer the walk stops at the first step.  Said once, with the count,
        # instead of leaving the reader to notice that many stacks are one
        # frame deep and guess why.
        unwalked = sum(1 for s in self.sites if not s.walked)
        if unwalked:
            out.append(("warn.check.nowalk",
                        {"n": unwalked, "total": len(self.sites)}))

        # Una pila CORTADA se lee igual que una entera, y en el arbol acaba
        # colgando de donde se corto en vez de de quien llamo.  Es la misma
        # razon por la que el propio comprobador la marca.
        cut = sum(1 for s in self.sites if getattr(s, "cut", False))
        if cut:
            out.append(("warn.check.cut", {"n": cut, "total": len(self.sites)}))

        # Y el resto de topes que el comprobador cuenta.  Cada uno se calla una
        # cosa distinta, asi que se dicen por separado en vez de sumarlos en un
        # "algo no se pudo": lo que falta solo sirve si se sabe QUE falta.
        for key, warn in (("frames_that_did_not_fit", "warn.check.poolfull"),
                          ("granules_that_did_not_fit", "warn.check.indexfull"),
                          ("pairs_that_did_not_fit", "warn.check.pairsfull"),
                          ("allocations_with_no_guard", "warn.check.noguard"),
                          ("chunks_with_no_shadow", "warn.check.noshadow")):
            n = int(self.summary.get(key, 0) or 0)
            if n:
                out.append((warn, {"n": n}))
        return out

    def level(self):
        """El nivel en vigor, o None si el volcado no lo dice.

        Lo dice `check_summary.csv`; una exportacion vieja no lo traia.
        """
        raw = self.summary.get("level")
        try:
            return int(raw)
        except (TypeError, ValueError):
            return None

    def missing_for_level(self):
        """Lo que ESTE nivel no puede contestar, como claves de texto.

        No es lo mismo que un aviso: un aviso dice que algo se intento y no
        cupo; esto dice que ni se intenta, porque el nivel pedido no lo produce.
        Sin esta distincion una seccion vacia se lee como "aqui no habia nada",
        que es justo la respuesta equivocada -- y la mas facil de creerse.
        """
        lvl = self.level()
        if lvl is None:
            return []
        out = []
        if lvl < 2:
            out.append("gap.level.canary")     # sin canario: no hay desbordes
        if lvl < 3:
            out.append("gap.level.poison")     # sin veneno: no hay uso-tras-liberar
        if lvl < 4:
            out.append("gap.level.guard")      # sin guarda: nada al instante
        if lvl >= 4:
            # El sombreado no se monta cuando los bloques salen de paginas
            # propias, asi que en este nivel no hay fugas ni vidas que ensenar.
            out.append("gap.level.noshadow")
        return out


class Report(object):
    """The five files, loaded."""

    def __init__(self, directory):
        self.directory = directory
        self.sites = []
        self.frames = {}      # site_id -> [Frame] indexed by depth
        self.summary = {}
        self.sizes = []
        self.tags = []
        # Every file exactly as it came, with its header: `raw[name]` are the
        # rows and `columns[name]` their order.  This is what lets the tool
        # show the export IN FULL instead of the part it has code for.
        self.raw = {}
        self.columns = {}
        self._load()

    def _read(self, name, optional=False):
        path = os.path.join(self.directory, name)
        if not os.path.isfile(path):
            # `optional` is for a file a NEWER export writes: an older one is
            # not broken, it just knew less.  Anything else missing means this
            # is not an export directory, and saying so beats loading half of
            # one and drawing a tree with holes in it.
            if optional:
                self.columns[name] = []
                self.raw[name] = []
                return []
            raise IOError("%s is missing: this is not an export directory"
                          % name)
        # `newline=""` is what the csv module asks for; without it a quoted
        # field containing a newline is split in two.
        with open(path, "r", newline="", encoding="utf-8",
                  errors="replace") as handle:
            reader = csv.DictReader(handle)
            rows = list(reader)
            self.columns[name] = list(reader.fieldnames or [])
            self.raw[name] = rows
            return rows

    def _load(self):
        """Lo que HAYA, y lo que no haya se nombra.

        Hasta aqui faltar `sites.csv` era "esto no es un directorio de
        exportacion" y se abortaba.  Eso da por hecho que quien exporta es
        siempre el mismo programa: el volcado del asignador lo dispara el
        anfitrion, mientras que el del comprobador se dispara solo, asi que
        cualquier programa que no sea ese anfitrion produce la mitad del
        comprobador -- completa y correcta -- y la herramienta la rechazaba
        entera diciendo que no era una exportacion.

        Media exportacion no es una rota: es una exportacion de lo que ese
        programa exporta.  Se carga lo que hay, y la mitad que falta se dice.
        Lo que no vale es cualquiera de los dos extremos: ni negarse, ni
        ensenar un arbol vacio como si el programa no reservara nada.
        """
        self.partial = []
        if not os.path.isfile(os.path.join(self.directory, "sites.csv")):
            # Sin la mitad del asignador, pero puede haber la del comprobador.
            self.partial.append("allocator")
            self.sites = []
            self.sizes = []
            self.tags = []
            self.check = self._load_check()
            if self.check is None:
                raise IOError("no hay ni sites.csv ni check_sites.csv: esto no "
                              "es un directorio de exportacion")
            return
        self.sites = [Site(r) for r in self._read("sites.csv")]
        for row in self._read("frames.csv"):
            sid = int(row["site_id"])
            depth = int(row["depth"])
            chain = self.frames.setdefault(sid, [])
            # The rows come in order, but never trust that: a hole would shift
            # the whole chain and the tree would hang branches off the wrong
            # parent without anything looking odd.
            while len(chain) <= depth:
                chain.append(None)
            chain[depth] = Frame(row["function"], row["file"],
                                 int(row["line"] or 0),
                                 row["inlined"] == "1",
                                 # `module` arrived later than the rest, so an
                                 # export written by an older build has no such
                                 # column.  That is unknown, not an error.
                                 row.get("module", ""))
        for sid, chain in self.frames.items():
            self.frames[sid] = [f for f in chain if f is not None]
        for row in self._read("summary.csv"):
            self.summary[row["key"]] = row["value"]
        self.sizes = self._read("sizes.csv")
        self.tags = self._read("tags.csv")
        # The per-site split of requested sizes.  Sparse -- only the buckets a
        # site actually used -- and written by newer exports only, so an older
        # one simply has none and every site's histogram is empty.
        by_site = {}
        # Y los bytes de cada casilla, de la misma fila.  Una exportacion
        # anterior no trae la columna, y eso es "no consta": el histograma se
        # queda con la vista de cuentas y lo dice, en vez de ensenar ceros.
        bytes_by_site = {}
        for row in self._read("site_sizes.csv", optional=True):
            sid = int(row["site_id"])
            bucket = int(row["bucket"])
            by_site.setdefault(sid, {})[bucket] = int(row["allocs"])
            if row.get("bytes"):
                bytes_by_site.setdefault(sid, {})[bucket] = int(row["bytes"])
        for site in self.sites:
            site.sizes = by_site.get(site.sid, {})
            site.size_bytes = bytes_by_site.get(site.sid, {})
        self.check = self._load_check()

    def _load_check(self):
        """The checker's export, when the run had it.  `None` when it did not.

        It is a DIFFERENT population, not more rows of the same one: the
        allocator's export is keyed by one return address and covers every
        block, while this one is keyed by a walked stack and only inspects what
        fits the shadow.  Merging them into one list would add up numbers that
        do not mean the same thing.
        """
        rows = self._read("check_sites.csv", optional=True)
        if not rows:
            return None
        sites = [CheckSite(r) for r in rows]

        # frame -> its inlining chain, then flattened innermost-first.  Built
        # in two steps because the rows are ordered by neither axis on their
        # own, and assuming an order here would hang branches off the wrong
        # parent without anything looking odd.
        by_stack = {}
        for row in self._read("check_frames.csv", optional=True):
            sid = int(row["stack_id"])
            frame = int(row.get("frame") or 0)
            depth = int(row.get("depth") or 0)
            by_stack.setdefault(sid, {}).setdefault(frame, {})[depth] = Frame(
                row["function"], row["file"], int(row["line"] or 0),
                row["inlined"] == "1", row.get("module", ""))
        chains = {}
        for sid, frames in by_stack.items():
            flat = []
            for fidx in sorted(frames):
                inner = frames[fidx]
                for d in sorted(inner):
                    flat.append(inner[d])
            chains[sid] = flat

        summary = {}
        for row in self._read("check_summary.csv", optional=True):
            summary[row["key"]] = row["value"]

        # Quien devuelve lo de quien.  Las dos columnas son ids de pila, los
        # mismos por los que se indexan los marcos de arriba, asi que esto se
        # une con `chains` sin nada por medio.
        pairs = {}
        for row in self._read("check_pairs.csv", optional=True):
            key = (int(row["alloc_stack"]), int(row["free_stack"]))
            pairs[key] = {"blocks": int(row["blocks"] or 0),
                          "bytes": int(row["bytes"] or 0)}

        # QUE ESTABA MAL.  Lo unico de la exportacion que no describe la corrida
        # sino sus fallos, y lo ultimo que salio de ser solo texto.  Las tres
        # pilas son ids, asi que se nombran con `chains` como cualquier otra.
        verdicts = [
            {"certainty": row.get("certainty", ""),
             "what": row.get("what", ""),
             "address": row.get("address", ""),
             "allocated": int(row.get("allocated") or 0),
             "released": int(row.get("released") or 0),
             "again": int(row.get("released_again") or 0)}
            for row in self._read("check_verdicts.csv", optional=True)]

        # CUANDO, que es el eje que a las tablas de arriba les falta.  Una fila
        # por corte, en el orden en que se tomaron: ese orden ES el tiempo, y
        # ordenar por otra cosa aqui convertiria una curva en un monton de
        # puntos.
        epochs = [{"i": int(row["epoch"]),
                   "mark": row.get("mark", ""),
                   "allocs": int(row["allocs"] or 0),
                   "live": int(row["live_bytes"] or 0),
                   "committed": int(row["committed_bytes"] or 0),
                   # Lo que cuestan los rangos DEL ASIGNADOR, preguntado al SO
                   # en el mismo corte.  Opcional a proposito: una tabla escrita
                   # por una version anterior no la trae, y leerla con `or 0`
                   # deja la curva sin dibujar en vez de reventar la lectura.
                   "region": int(row.get("region_bytes") or 0),
                   # De la region, lo que esta parado en las listas de libres.
                   # El resto del hueco son trozos que una clase de tamano
                   # retiene a medio usar, y esa es la diferencia que decide
                   # que arreglo toca.  Opcional, como la de arriba.
                   "free_spans": int(row.get("free_span_bytes") or 0),
                   # Y de lo que no son tramos libres, los trozos de clase
                   # a los que se les murio TODO: esos tambien se pueden
                   # devolver, y son la mayor de las tres partes.
                   "empty_chunks": int(row.get("empty_chunk_bytes") or 0),
                   "born": int(row["born_bytes"] or 0),
                   "died": int(row["died_bytes"] or 0),
                   "other": int(row.get("below_floor_bytes") or 0)}
                  for row in self._read("check_epochs.csv", optional=True)]
        epochs.sort(key=lambda e: e["i"])
        # Disperso a proposito: un sitio solo aparece en los cortes en los que
        # tenia algo por encima del suelo.  Rellenar los huecos con ceros aqui
        # multiplicaria la tabla por los cientos de sitios que casi nunca
        # tienen nada, y quien dibuja ya sabe que un hueco es un cero.
        series = {}
        for row in self._read("check_epoch_sites.csv", optional=True):
            series.setdefault(int(row["stack_id"]), {})[
                int(row["epoch"])] = int(row["live_bytes"] or 0)
        # LA MISMA CURVA, POR TAMANO: casilla -> {corte: bytes vivos}.  No sale
        # de la de arriba -- un sitio sirve varias casillas, y ademas aquella
        # solo lleva los sitios que pasan del suelo --, asi que es una medida
        # propia y no una vista de la otra.
        size_series = {}
        for row in self._read("check_epoch_sizes.csv", optional=True):
            size_series.setdefault(int(row["bucket"]), {})[
                int(row["epoch"])] = int(row["live_bytes"] or 0)
        peak = [{"stack": int(row["stack_id"]),
                 "bytes": int(row["live_bytes"] or 0),
                 "share": int(row.get("share_per_mille") or 0)}
                for row in self._read("check_peak.csv", optional=True)]
        peak.sort(key=lambda r: -r["bytes"])
        # QUE SE QUEDA CADA TAMANO.  `live` viene VACIO, y no a cero, cuando la
        # corrida no pudo saberlo: un bloque demasiado grande para el sombreado
        # solo se reconoce al volver por la tabla que lleva el eje, asi que sin
        # eje nace y no muere nunca.  `None` es "no consta" y la pagina lo dice;
        # un cero seria "este tamano no se queda nada", que es lo contrario.
        sizes = [{"bucket": int(row["bucket"]),
                  "upper": int(row["upper_bytes"] or 0),
                  "allocs": int(row["allocs"] or 0),
                  "bytes": int(row["bytes"] or 0),
                  "frees": int(row["frees"] or 0),
                  "freed": int(row["freed_bytes"] or 0),
                  "live": int(row["live_bytes"]) if row.get("live_bytes")
                  else None,
                  "live_max": int(row["live_bytes_max"])
                  if row.get("live_bytes_max") else None}
                 for row in self._read("check_sizes.csv", optional=True)]
        sizes.sort(key=lambda r: r["bucket"])
        return CheckReport(sites, chains, summary, pairs, verdicts, epochs,
                           series, peak, sizes, size_series)

    def chain_of(self, site):
        """The chain of a site, never empty.

        With no names it is still a place -- the offset -- and it belongs in
        the tree as such.  Dropping it would quietly shrink the totals, which
        is the one thing a profiler must not do.
        """
        chain = self.frames.get(site.sid)
        if chain:
            return chain
        return [Frame("+0x%x" % site.pc, "", 0, False)]

    def warnings(self):
        """What could NOT be done, as (key, parameters).

        A tree with no names looks like a program that allocates from nowhere,
        and a truncated one looks like a program with fewer sites.  Neither is
        true, and neither is visible unless it is said.

        Keys and not sentences, because the same warning has to come out on a
        terminal in the language asked for on the command line and on a page
        whose reader switches language without reloading.  A formatted
        sentence can only do the first.
        """
        out = []
        if self.summary.get("has_symbols") != "1":
            out.append(("warn.nosymbols", {}))
        missing = int(self.summary.get("sites_without_frames", 0))
        if missing:
            out.append(("warn.unresolved", {"n": missing}))
        sites = int(self.summary.get("sites", 0))
        cap = int(self.summary.get("sites_capacity", 0))
        if cap and sites >= cap:
            out.append(("warn.full", {"n": cap}))
        evicted = int(self.summary.get("evicted", 0))
        if evicted:
            out.append(("warn.evicted", {"n": evicted}))
        deep = int(self.summary.get("deepest_chain", 0))
        cap = int(self.summary.get("frames_capacity", 0))
        if cap and deep >= cap:
            out.append(("warn.deep", {"n": cap}))
        return out

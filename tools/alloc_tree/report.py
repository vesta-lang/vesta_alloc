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
                 "classes", "large", "pc", "row", "mask", "sizes", "foreign")

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
                 "classes", "large", "pc", "row", "mask", "sizes", "foreign",
                 "weighed_allocs", "weighed_bytes", "deaths", "alive_blocks",
                 "alive_bytes", "life_avg", "life_max", "cross_thread",
                 "size_min", "size_max", "use", "shape", "walked", "frames")

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
        self.foreign = False
        self.pc = 0
        # The purpose, MEASURED.  The allocator's `tag` is what the programmer
        # declared; this is what the blocks turned out to be.  They go in the
        # same field so that `--group purpose` and the purpose column work on
        # both exports -- and the header says which one is being shown, because
        # confusing intent with outcome is exactly the mistake worth avoiding.
        self.tag = ("%s/%s" % (self.use, self.shape)) if self.use else ""


class CheckReport(object):
    """The checker's three files, in the shape the rest of the tool expects.

    `chain_of` is where the two exports really differ.  The allocator records
    ONE return address and resolves its inlining chain; the checker walks the
    stack, so it has TWO axes -- `frame`, the call frame, and `depth`, the
    inline level inside it.  Flattening them innermost-first gives exactly what
    the tree builder already consumes, and keeps the distinction visible in the
    frame itself, so nobody has to line up two tables by eye.
    """

    def __init__(self, sites, chains, summary):
        self.sites = sites
        self.chains = chains          # stack_id -> [Frame], innermost first
        self.summary = summary

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
        for row in self._read("site_sizes.csv", optional=True):
            by_site.setdefault(int(row["site_id"]), {})[
                int(row["bucket"])] = int(row["allocs"])
        for site in self.sites:
            site.sizes = by_site.get(site.sid, {})
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
        return CheckReport(sites, chains, summary)

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

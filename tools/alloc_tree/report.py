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

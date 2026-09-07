# -*- coding: utf-8 -*-
#
# VestaVM -- Distributed Virtual Machine
#
# Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
# License: MIT (see LICENSE).  Part of the VestaVM family.
"""Merging every site's chain into one tree.

TWO DIRECTIONS, and they answer different questions:

    top-down    from the function that exists in the binary INWARDS, through
                everything the optimiser inlined into it.  "Where does this
                allocation come from."
    bottom-up   from where the allocation physically happens OUTWARDS.  "Who
                ends up calling `operator new` the most" -- this is the one
                that groups all the `std::vector` growth together.

The page builds its own copy of this in the browser, so flipping the direction
costs nothing there.  This one serves `--text`, and is what the tests exercise.
"""
from .report import Frame


class Node(object):
    """A function in the tree, with what went through it."""

    __slots__ = ("frame", "allocs", "bytes", "own_allocs", "own_bytes",
                 "children", "sites", "tags", "over")

    def __init__(self, frame):
        self.frame = frame
        self.allocs = 0
        self.bytes = 0
        self.own_allocs = 0   # what ENDS here, not what passes through
        self.own_bytes = 0
        self.children = {}
        # The sites whose chain ENDS here, by id.  Carrying them is what lets
        # a row be opened up into the raw rows it was folded from: the tree is
        # a summary, and a summary with no way back to the measurement asks to
        # be taken on faith.
        self.sites = []
        # What this branch was FOR, by declared purpose.  It travels with the
        # node instead of living in a table of its own: "who allocated" and
        # "what for" are the same question asked twice, and answering them in
        # two places that cannot be lined up answers neither.
        self.tags = {}
        # What this branch INHERITED by evicting a weaker entry.  Kept apart
        # from `allocs` on purpose: folding them together would produce a
        # number that looks exact and is an upper bound.
        self.over = 0

    def child(self, frame):
        # Keyed by function and file, NOT by line.  The line is where the CALL
        # is, so it differs from one site to the next inside the same function
        # -- and with it in the key the same function came out as several
        # sibling rows with the same name, which is not a tree of functions,
        # it is a tree of call sites wearing function names.
        key = (frame.function, frame.file)
        node = self.children.get(key)
        if node is None:
            node = Node(frame)
            self.children[key] = node
        return node

    def sorted_children(self, by_bytes=False):
        key = (lambda n: -n.bytes) if by_bytes else (lambda n: -n.allocs)
        return sorted(self.children.values(), key=key)


def group_frame(site, chain, how):
    """The extra level that goes ON TOP of the chain, or None for no grouping.

    Grouping is a level, not a different tree: the chain underneath is the
    same one, so folding a group open leads to exactly the rows that were
    there before.  A grouping that rebuilt the tree differently would make the
    two views impossible to compare.
    """
    if how == "module":
        # The OUTERMOST frame is the one that exists in the binary, so its
        # module is whose code asked -- which is the question.  The innermost
        # would answer "std::string", which is true and useless.
        for frame in reversed(chain):
            if frame.module:
                return Frame(frame.module, "", 0, False, frame.module)
        return Frame("(sin clasificar)", "", 0, False)
    if how == "file":
        for frame in reversed(chain):
            if frame.file:
                return Frame(frame.file, frame.file, 0, False, frame.module)
        return Frame("(sin fichero)", "", 0, False)
    if how == "purpose":
        return Frame(site.tag or "(sin declarar)", "", 0, False)
    return None


def build_tree(report, top_down, needle="", group=None):
    """The tree, with EVERYTHING in it.

    There is no filtering here on purpose.  Deciding to look at only the
    program's own calls is a way of LOOKING, and it belongs where looking
    happens -- in the page, which can turn it on and off without losing the
    measurement.  What this side does is carry the facts that make the filter
    possible: see `page.py`, which marks each frame and each site.
    """
    root = Node(Frame("(everything)", "", 0, False))
    for site in report.sites:
        chain = report.chain_of(site)
        if needle and not any(needle in f.function.lower()
                              or needle in f.file.lower() for f in chain):
            continue
        path = list(reversed(chain)) if top_down else list(chain)
        if group:
            head = group_frame(site, chain, group)
            if head is not None:
                path = [head] + path
        root.allocs += site.allocs
        root.bytes += site.bytes
        root.over += site.over
        root.tags[site.tag] = root.tags.get(site.tag, 0) + site.allocs
        node = root
        for frame in path:
            node = node.child(frame)
            node.allocs += site.allocs
            node.bytes += site.bytes
            node.over += site.over
            node.tags[site.tag] = node.tags.get(site.tag, 0) + site.allocs
        node.own_allocs += site.allocs
        node.own_bytes += site.bytes
        node.sites.append(site.sid)
    return root


def human_bytes(n):
    for unit in ("B", "KiB", "MiB", "GiB"):
        if unit == "B":
            if n < 1024:
                return "%d B" % n
        elif n < 1024 or unit == "GiB":
            return "%.1f %s" % (n, unit)
        n /= 1024.0
    return "%d B" % n


def print_tree(node, total, depth, limit, out):
    """The same tree, written out, for a terminal.

    By default it prints the WHOLE tree.  `limit` is there for someone who
    asks to fold the small branches away, and when it does fold it SAYS so:
    a tree that ends without a word reads like a program that stopped
    allocating there.

    The name goes LAST and unabridged.  A C++ symbol is long, and cutting it
    to make the columns line up takes away exactly the part that tells two
    instantiations of the same template apart -- and leaves nothing to paste
    into a search either.
    """
    children = node.sorted_children()
    for i, child in enumerate(children):
        pct = 100.0 * child.allocs / (total or 1)
        if limit > 0 and pct < limit:
            out.write("%s... %d more branches under %.1f%% (raise --limit or "
                      "pass 0 to see them)\n"
                      % ("  " * depth, len(children) - i, limit))
            break
        where = child.frame.where()
        # The purposes travel WITH the row, all of them.  A node usually has
        # one or two, and putting them in a table of their own would mean the
        # reader has to line up two lists by eye to answer "what was this
        # branch for" -- which is the question the tag system exists for.
        purposes = ",".join(sorted(child.tags, key=lambda t: -child.tags[t]))
        out.write("%s%8d %6.2f%% %8s %8d %-16s %-18s %s%s%s\n"
                  % ("  " * depth, child.allocs, pct,
                     human_bytes(child.bytes), child.own_allocs,
                     purposes, child.frame.module, child.frame.function,
                     "   " + where if where else "",
                     "   [inlined]" if child.frame.inlined else ""))
        print_tree(child, total, depth + 1, limit, out)

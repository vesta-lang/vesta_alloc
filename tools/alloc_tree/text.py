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
    ("check_summary.csv", "file.check_summary"),
)


# How wide a column is allowed to get.  One four-hundred-character symbol in a
# table makes its column four hundred wide and pushes everything after it off
# the screen for every OTHER row.  A value longer than this is still printed
# WHOLE -- it just runs over its column on its own line, which costs the
# alignment of that one row instead of the alignment of all of them.
MAX_COLUMN = 60


def print_table(columns, rows, out):
    """Every row, every column, aligned.

    The width of a column is the width of its widest VALUE -- computed, not
    guessed -- so nothing has to be shortened to fit a number decided in
    advance.  Capped at `MAX_COLUMN`, which is a limit on the COLUMN and never
    on the value.
    """
    if not rows:
        out.write("    (no rows)\n")
        return
    width = [len(c) for c in columns]
    table = []
    for row in rows:
        cells = [str(row.get(c, "")) for c in columns]
        for i, cell in enumerate(cells):
            if len(cell) > width[i]:
                width[i] = min(len(cell), MAX_COLUMN)
        table.append(cells)
    line = "  ".join(c.ljust(width[i]) for i, c in enumerate(columns))
    out.write("    %s\n" % line)
    out.write("    %s\n" % "  ".join("-" * w for w in width))
    for cells in table:
        out.write("    %s\n"
                  % "  ".join(c.ljust(width[i]) for i, c in enumerate(cells)))


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
    out.write("\n")
    print_tree(tree, tree.allocs, 0, limit, out)


def print_header(report, tree, out, lang="en"):
    summary = report.summary
    out.write("\n%s\n" % t(lang, "head.totals",
                           allocs="{:,}".format(tree.allocs),
                           sites="{:,}".format(len(report.sites)),
                           bytes=human_bytes(
                               int(summary.get("bytes_reserved", 0) or 0))))
    for key, params in report.warnings():
        out.write("WARNING: %s\n" % t(lang, key, **params))


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

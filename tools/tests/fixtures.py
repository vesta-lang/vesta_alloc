# -*- coding: utf-8 -*-
#
# VestaVM -- Distributed Virtual Machine
#
# Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
# License: MIT (see LICENSE).  Part of the VestaVM family.
"""An export written by hand, so the tests do not need a build.

WHY SYNTHETIC AND NOT A REAL RUN.  Because a test that needs a compiled
program to produce its input is a test that stops being run: it fails on a
machine with no toolchain, it changes every time the program changes, and
whoever sees it red has to work out which of the two is broken.  Here every
number is known in advance, so a failure means the TOOL is wrong.

The awkward cases are in on purpose, because they are the ones that broke
while this was being written: a name with commas and quotes in it, the same
function allocating from two different lines, a chain with inlined frames, two
purposes, and a site with no frames at all.
"""
import os

# A name with everything that breaks a CSV reader and a column layout at once.
NASTY = ('std::pair<std::string const, std::vector<Foo, "bar">>::pair'
         '(int, char)')

SITES = [
    # id  allocs bytes  over overB large classes mask tag  tag_name        pc   foreign
    (0, 100, 6400, 0, 0, 0, 1, 2, 5, "instant/fixed", 4096, 0),
    (1, 50, 3200, 5, 320, 0, 1, 2, 5, "instant/fixed", 4128, 0),
    (2, 25, 40000, 0, 0, 3, 2, 12, 14, "long/growing", 8192, 0),
    (3, 10, 640, 0, 0, 0, 1, 4, 0, "unknown", 12288, 0),
    # A site with NO frames: the resolver knew nothing about it.  It still has
    # to appear in the tree, or the totals quietly shrink.
    (4, 7, 448, 0, 0, 0, 1, 4, 0, "unknown", 65536, 0),
    # A site in ANOTHER MODULE: the C runtime allocating inside itself.  Its
    # offset is measured from OUR base, so it is a huge meaningless number --
    # which is exactly why the column next to it exists.
    (5, 9, 450, 0, 0, 0, 1, 4, 0, "unknown", 30534573892, 1),
    # C PURO: un solo marco, en un `.c`.  Hace falta a proposito -- sin el, el
    # filtro por lenguaje no se podria probar en la direccion "solo C", que es
    # justo la mitad que se olvida.
    (6, 12, 768, 0, 0, 0, 1, 2, 0, "unknown", 16384, 0),
]

# (site, depth, inlined, function, file, line, module).  Innermost first.
#
# Sites 0 and 1 are the SAME function at two different lines: that is what
# used to come out as two sibling rows with one name.
FRAMES = [
    (0, 0, 1, NASTY, "src/util/alloc,report.cpp", 42, "util"),
    (0, 1, 0, "vx_parse_file", "src/vx/parser.c", 100, "vx"),
    (1, 0, 1, NASTY, "src/util/alloc,report.cpp", 77, "util"),
    (1, 1, 0, "vx_parse_file", "src/vx/parser.c", 130, "vx"),
    (2, 0, 0, "std::vector<int>::_M_realloc_insert<int>(int&&)",
     "include/c++/12/bits/vector.tcc", 440, "libstdc++"),
    (3, 0, 0, "main", "src/main.cpp", 10, ""),
    # The foreign one: a name and a module, and no file or line, because the
    # debug information of somebody else's library is not there to read.
    (5, 0, 0, "realloc", "", 0, "C:\\Windows\\System32\\msvcrt.dll"),
    (6, 0, 0, "vx_lexer_token", "src/vx/lexer.c", 88, "vx"),
]

# (site, bucket, upper_bytes, allocs).  Sparse, and it must SUM to each site's
# `allocs` -- that is the invariant tying the two tables together, and the one
# that fails silently if it ever drifts.
SITE_SIZES = [
    (0, 0, 64, 40), (0, 1, 256, 60),
    (1, 1, 256, 50),
    (2, 9, 1048576, 25),
    (3, 0, 64, 10),
    (4, 0, 64, 7),
]

SIZES = [(i, limit, allocs) for i, (limit, allocs) in enumerate(
    [(64, 60), (256, 90), (1024, 25), (2048, 0), (4096, 0), (8192, 0),
     (16384, 0), (65536, 3), (262144, 0), (1048576, 0), (16777216, 0),
     (0, 0)])]

TAGS = [(5, "instant/fixed", 150), (14, "long/growing", 25), (0, "unknown", 17)]

SUMMARY = [
    ("sites", 5), ("sites_capacity", 4096), ("small_allocs", 192),
    ("small_frees", 100), ("remote_frees", 0), ("large_allocs", 3),
    ("large_frees", 0), ("chunks", 7), ("bytes_reserved", 262144),
    ("untagged", 17), ("evicted", 5), ("module_base", 140700000000000),
    ("has_symbols", 1), ("sites_without_frames", 1), ("deepest_chain", 2),
    ("frames_capacity", 64),
]


def _quote(value):
    """The same rule the exporter follows: quote when it needs it."""
    text = str(value)
    if any(c in text for c in ',"\n\r'):
        return '"' + text.replace('"', '""') + '"'
    return text


def _write(path, header, rows):
    with open(path, "w", newline="", encoding="utf-8") as handle:
        handle.write(",".join(header) + "\n")
        for row in rows:
            handle.write(",".join(_quote(v) for v in row) + "\n")


def write_export(directory, mask=True, module=True, site_sizes=True,
                 foreign=True):
    """Writes the export.

    @param mask       include `class_mask`; False writes what an OLDER build
                      wrote, which the tool still has to load.
    @param module     include the `module` column of `frames.csv`, same reason.
    @param site_sizes write `site_sizes.csv` at all; False is again an older
                      export, and its absence is not an error.
    @param foreign    include the `foreign` column of `sites.csv`.  False is an
                      export from before the allocator could serve the C
                      runtime from the inside -- back then nothing but the
                      program could allocate, so "missing" means "not foreign"
                      and not "unknown".
    """
    if not os.path.isdir(directory):
        os.makedirs(directory)

    head = ["site_id", "allocs", "bytes", "over_allocs", "over_bytes", "large",
            "size_classes"]
    if mask:
        head.append("class_mask")
    head += ["tag", "tag_name", "pc_offset"]
    if foreign:
        head.append("foreign")
    rows = [r if mask else r[:7] + r[8:] for r in SITES]
    if not foreign:
        rows = [r[:-1] for r in rows]
    _write(os.path.join(directory, "sites.csv"), head, rows)

    head = ["site_id", "depth", "inlined", "function", "file", "line"]
    if module:
        head.append("module")
    _write(os.path.join(directory, "frames.csv"), head,
           [f if module else f[:6] for f in FRAMES])

    _write(os.path.join(directory, "sizes.csv"),
           ["bucket", "upper_bytes", "allocs"], SIZES)
    if site_sizes:
        _write(os.path.join(directory, "site_sizes.csv"),
               ["site_id", "bucket", "upper_bytes", "allocs"], SITE_SIZES)
    _write(os.path.join(directory, "tags.csv"),
           ["tag", "tag_name", "allocs"], TAGS)
    _write(os.path.join(directory, "summary.csv"), ["key", "value"], SUMMARY)


# What the tests check against, computed from the tables above so the two
# cannot drift apart.
TOTAL_ALLOCS = sum(s[1] for s in SITES)
TOTAL_BYTES = sum(s[2] for s in SITES)
TOTAL_OVER = sum(s[3] for s in SITES)

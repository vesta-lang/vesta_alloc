# -*- coding: utf-8 -*-
#
# VestaVM -- Distributed Virtual Machine
#
# Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
# License: MIT (see LICENSE).  Part of the VestaVM family.
"""The export as a page: a tree grid, and the tables it was folded from.

WHY A GRID AND NOT A CHART.  Because what has to be shown is a call path with
numbers next to it -- the full name, the total, what the frame did on its own,
whose module it is, which file -- and that is a table with indentation, the way
every profiler worth the name shows a top-down tree.  A rectangle chart looks
impressive and answers none of it: a four-hundred-character C++ name does not
fit in a rectangle, and cutting it throws away the template arguments, which
are the part that tells two instantiations apart.

AND NOTHING IS LEFT OUT.  Under the tree go the five files of the export, whole
-- every row, every column, including columns this tool has no code for.  A
tool that shows part of a measurement makes the rest impossible to look for,
and the reader cannot tell which part is missing.

WHY JINJA2 AND NOT A STRING WITH HTML IN IT.  Because this is a page generator,
and one written by concatenating strings is a page generator that escapes
nothing: a function name carrying `<` closes a tag and quietly eats the rest of
the row.  Jinja escapes by default, the markup lives in a template a person can
read, and the stylesheet and the script are files an editor understands.
"""
import json
import os

from .i18n import LANGS, STRINGS
from .ours import is_library_frame
from .text import CHECK_FILES, FILES
from .tree import human_bytes

TEMPLATES = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         "templates")

MISSING = ("this needs Jinja2, which is what turns the template into the "
           "page:\n"
           "    python -m pip install jinja2\n"
           "The export still reads without it: pass --text for the same tree "
           "and the same tables on the terminal.")


def _numeric(rows, column):
    """Is this column a number, judging by what is IN it.

    Asked of the data and not of a list written here, so a column added to the
    export later still lines up and still sorts as a number.  An empty column
    is not a number: aligning unknowns to the right only looks like data.
    """
    seen = False
    for row in rows:
        value = row.get(column, "")
        if value == "":
            continue
        seen = True
        try:
            float(value)
        except ValueError:
            return False
    return seen


def _tables(report):
    """The five files, whole, in the order they are worth reading.

    The file NAME is not translated -- it is what the file is called on disk,
    and renaming it in the page would break the only link between what is on
    screen and what is in the directory.  What is translated is what it holds.
    """
    out = []
    # The checker's three go LAST and only when the run had them.  Last because
    # they are a different population and reading them first invites adding
    # their numbers to the ones above; only when present because three empty
    # tables read like an export that lost them.
    names = FILES + (CHECK_FILES if report.check else ())
    for name, key in names:
        rows = report.raw.get(name, [])
        columns = report.columns.get(name, [])
        out.append({
            "id": name.split(".")[0],
            "file": name,
            "note_key": key,
            "columns": [{"name": c,
                         "numeric": _numeric(rows, c),
                         # The two that carry code: monospaced, because a name
                         # and a path are read character by character.
                         "mono": c in ("function", "file")}
                        for c in columns],
            "rows": [[row.get(c, "") for c in columns] for row in rows],
        })
    return out


def _payload(report):
    """The MEASUREMENT, not a tree built from it.

    WHY NOT SHIP THE TREE.  Because there is more than one tree.  The same
    sites can be read from the binary inwards or from the allocation outwards,
    and grouped first by module, by source file or by declared purpose -- and
    which of those answers "who allocated this" depends on what is being
    asked.  Precomputing them would mean eight trees in the file, most of the
    names repeated in each, and still no answer for the ninth way somebody
    wants to look at it.

    So what travels is what was measured: the frames, interned so each name is
    written ONCE, and the sites pointing into them.  The browser folds the tree
    it needs.  It is also far smaller: the same export went from 2.4 MiB of
    precooked trees to a fraction of that, because in a call tree the same
    function name appears under dozens of branches.
    """
    data = _dataset(report, report.sites, report.chain_of,
                    report.columns.get("sites.csv", []))
    # What each bucket means, so the page can label the split without a second
    # copy of the boundaries -- they come from the export, which took them
    # from the allocator.
    data["buckets"] = [[int(r["bucket"]), int(r["upper_bytes"])]
                       for r in report.sizes]
    # The checker's population, built the SAME way and kept apart.  The page
    # switches between the two with one control and folds the tree it needs
    # with the same code, which is the whole reason for giving them one shape:
    # a second renderer would drift from the first the day one of them gains a
    # column.
    if report.check:
        check = _dataset(report, report.check.sites, report.check.chain_of,
                         report.columns.get("check_sites.csv", []))
        check["buckets"] = []
        data["check"] = check
    return dict(data,
                # The whole catalogue travels, not the chosen language: the
                # reader who wants the other one is not going to run the tool
                # again, and a page that has to be regenerated to change
                # language is a page that stays in the language of whoever
                # generated it.
                strings=STRINGS, langs=[list(l) for l in LANGS],
                meta=_meta(report),
                # The checker's warnings travel with the allocator's, in one
                # list.  They are about the same run, and splitting them into
                # two boxes would let a reader dismiss one box and miss that
                # half of the bytes were only weighed.
                warnings=[[k, p] for k, p in report.warnings()]
                + ([[k, p] for k, p in report.check.warnings()]
                   if report.check else []))


def _dataset(report, site_list, chain_of, site_columns):
    """One population, interned: the frames written once and the sites into
    them.

    Taken out of `_payload` so the checker's export goes through exactly this
    code and not a copy of it.  The two differ in what they measured, never in
    how they are drawn.
    """
    frames = []
    index = {}

    def intern(frame):
        key = (frame.function, frame.file, frame.line, frame.module,
               frame.inlined)
        at = index.get(key)
        if at is None:
            at = len(frames)
            index[key] = at
            frames.append([frame.function, frame.file, frame.line,
                           frame.module, 1 if frame.inlined else 0,
                           # WHETHER THIS FRAME IS LIBRARY CODE, decided HERE
                           # and not in the browser.  The page needs it to
                           # offer "only my own calls", and the rule for
                           # deciding it -- names, header paths -- is a
                           # judgement call that belongs in one place with its
                           # reasons written down (`ours.py`), not copied into
                           # JavaScript where it would drift.
                           1 if is_library_frame(frame) else 0])
        return at

    sites = []
    for site in site_list:
        chain = chain_of(site)
        sites.append({
            "id": site.sid,
            # Innermost first, the order the export writes them in.
            "chain": [intern(f) for f in chain],
            "allocs": site.allocs,
            "bytes": site.bytes,
            # Apart from `allocs` on purpose: what a site inherited by
            # evicting a weaker entry makes its count an upper bound, and
            # folding the two together would hide that.
            "over": site.over,
            "overBytes": site.over_bytes,
            "tag": site.tag,
            "large": site.large,
            # In two halves of 32 bits because a JavaScript number stops being
            # exact above 2^53, and these are 64 bits of flags: OR-ing them as
            # one number would silently drop the top classes -- the big ones,
            # which are the ones worth seeing.
            "mask": [site.mask & 0xFFFFFFFF, (site.mask >> 32) & 0xFFFFFFFF],
            # bucket -> allocations, sparse.  Summed up the tree so a branch
            # can answer "many small or a few large", which the process-wide
            # histogram cannot: a global split cannot be handed back out to
            # the sites that formed it.
            "sizes": {str(b): n for b, n in sorted(site.sizes.items())},
            "off": "0x%x" % site.pc,
            # In a module that is NOT the program: the C runtime, a system
            # library.  A FACT from the export and not a guess -- from out here
            # a shared library and a logical module of the project look the
            # same.  The page uses it for the "only my own calls" filter, and
            # it also explains the offset: for a foreign site the displacement
            # is measured from OUR base, so it means nothing.
            "foreign": 1 if site.foreign else 0,
            # The whole row, so a tree row can be opened into the measurement
            # it was folded from without another lookup table.
            "row": [site.row.get(c, "") for c in site_columns],
        })
    return {"frames": frames, "sites": sites, "siteCols": site_columns,
            "totals": {"allocs": sum(s["allocs"] for s in sites),
                       "bytes": sum(s["bytes"] for s in sites)}}


def _meta(report):
    """The totals as NUMBERS, for the page to word in whichever language.

    Formatted here they would be one language for ever, and the reader who
    switches would get a translated page with an English headline on it.
    """
    summary = report.summary
    return {
        "sites": summary.get("sites", "?"),
        "allocs": summary.get("small_allocs", "?"),
        "bytes": human_bytes(int(summary.get("bytes_reserved", 0) or 0)),
        "unresolved": summary.get("sites_without_frames", "0"),
    }


def _read(name):
    with open(os.path.join(TEMPLATES, name), "r", encoding="utf-8") as handle:
        return handle.read()


def write_page(report, build_tree, out_path, title):
    """Writes the self-contained page.  Returns its size in bytes.

    `build_tree` is not used any more -- the page folds its own trees now --
    and stays in the signature because the caller passes it and because the
    terminal view still needs it.  It is one argument, not a reason to make
    two call shapes for the same thing.
    """
    from jinja2 import Environment, FileSystemLoader
    from markupsafe import Markup

    # `autoescape=True` and not by extension: this page is fed function names
    # straight out of a symbol table, and one containing `<` would close a tag
    # and swallow the rest of the row.  Escaping is the default and the three
    # things that must NOT be escaped are marked one by one, right here.
    env = Environment(loader=FileSystemLoader(TEMPLATES), autoescape=True,
                      trim_blocks=True, lstrip_blocks=False)
    data = json.dumps(_payload(report), separators=(",", ":"))
    # A `</script>` inside a string would end the block early, and the rest of
    # the data would be parsed as HTML.  It cannot happen with our own names,
    # but the data comes from whatever was linked, so it is not left to luck.
    data = data.replace("</", "<\\/")

    tables = _tables(report)
    page = env.get_template("page.html.j2").render(
        title=title,
        langs=LANGS,
        tables=tables,
        # Whether to offer the population switch at all.  Not a disabled
        # control: one that offers something the file does not carry is a
        # promise the page cannot keep.
        has_check=bool(report.check),
        raw_rows=sum(len(t["rows"]) for t in tables),
        css=Markup(_read("page.css")),
        # ORDER MATTERS: `page.js` calls into `tables.js` while it starts up,
        # and a classic script that is not there yet is not an error you see --
        # the tree draws, the tables underneath just never sort.
        # ORDER MATTERS: `code.js` defines the escaping and the symbol
        # painting that the others use, and `page.js` calls into all of them
        # while it starts up.  A classic script that is not there yet is not
        # an error you see -- the page just comes up half-drawn.
        scripts=[Markup(_read("code.js")), Markup(_read("i18n.js")),
                 Markup(_read("tables.js")), Markup(_read("tree.js")),
                 Markup(_read("link.js")), Markup(_read("detail.js")),
                 Markup(_read("page.js"))],
        data=Markup(data))
    with open(out_path, "w", encoding="utf-8") as handle:
        handle.write(page)
    return os.path.getsize(out_path)

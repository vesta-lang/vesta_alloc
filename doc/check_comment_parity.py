#!/usr/bin/env python3
"""Comment parity: every substantial comment carries both languages.

\\~english
WHY THIS EXISTS.  `check_parity.py` compares the two Doxygen builds, so it sees
what Doxygen sees -- the headers.  The prose inside the implementation files is
invisible to it, and that is exactly where the rule fell apart: 67 files and
8476 lines of comment in ONE language, drifting for as long as nobody looked.
A rule with nothing checking it comes undone on its own, which is the failure
mode this project chases everywhere else.

WHAT IT ASKS.  A comment block of `MIN_LINES` lines or more must carry both
`\\~english` and `\\~spanish`.  One- and two-line notes are exempt on purpose:
demanding a translation of `// see above` buys nothing and would bury the real
findings under noise.

HOW IT FAILS.  Not with a list of exceptions -- that is how a checker starts
lying, one entry at a time -- but with a CEILING that can only come down.  The
number in `CEILING` is what was left the last time somebody looked; the build
goes red the moment it grows.  Nothing is hidden: the whole list is printed
every run, and the number says how far there is to go.

\\~spanish
POR QUE EXISTE.  `check_parity.py` compara las dos referencias de Doxygen, asi
que ve lo que ve Doxygen: las cabeceras.  La prosa de los ficheros de
implementacion le es invisible, y ahi es justo donde la regla se deshizo: 67
ficheros y 8476 lineas de comentario en UN idioma, derivando mientras nadie
miraba.  Una regla sin quien la compruebe se deshace sola, que es el modo de
fallo que este proyecto persigue en todo lo demas.

QUE PIDE.  Un bloque de comentario de `MIN_LINES` lineas o mas tiene que llevar
`\\~english` y `\\~spanish`.  Los de una o dos lineas quedan fuera a proposito:
exigir traduccion de `// ver arriba` no compra nada y enterraria los hallazgos
de verdad debajo del ruido.

COMO FALLA.  No con una lista de excepciones -- asi es como un comprobador
empieza a mentir, una entrada cada vez -- sino con un TECHO que solo puede
bajar.  El numero de `CEILING` es lo que quedaba la ultima vez que alguien
miro; el build se pone rojo en cuanto sube.  No se esconde nada: la lista entera
se imprime en cada corrida, y el numero dice cuanto falta.
\\~
"""

import os
import re
import sys

# A comment shorter than this is exempt.  Three lines is where a comment stops
# being a label and starts being an explanation.
MIN_LINES = 3

# What was left the last time somebody looked.  It may go DOWN and never up.
# Lower it in the same commit that fixes the blocks; that is what keeps it
# honest.
CEILING = 961

ROOTS = ("src", "tests", "bench", "examples", "include", "support")
SUFFIXES = (".c", ".cc", ".cpp", ".h", ".hpp")

BLOCK = re.compile(r"/\*.*?\*/", re.S)
LINE_RUN = re.compile(r"(?:^[ \t]*//[^\n]*\n)+", re.M)


# A block that is only the licence notice.  Demanding a translation of it would
# be the checker crying wolf on the one comment in the file that says nothing
# about the code -- and a checker that cries wolf gets switched off.
LICENCE = re.compile(r"Copyright \(C\)|License:|SPDX-License")


# What is left of a line once the comment markers and the rules are taken off.
# A banner of three lines whose middle one is the only prose is a LABEL, not an
# explanation, and asking for it in two languages triples the banner to say the
# same thing twice.  So what counts is lines that SAY something.
MARKERS = re.compile(r"^[ \t]*(?:/\*+|\*+/|\*|//+|///+)?[ \t]*")
RULE = re.compile(r"^[=\-*_ \t]*$")


def prose_lines(body):
    n = 0
    for raw in body.splitlines():
        stripped = MARKERS.sub("", raw).rstrip()
        if stripped.endswith("*/"):
            stripped = stripped[:-2].rstrip()
        if stripped and not RULE.match(stripped):
            n += 1
    return n


def offending_blocks(text):
    """Yields (line number, how many lines) for each block missing a language."""
    for match in list(BLOCK.finditer(text)) + list(LINE_RUN.finditer(text)):
        body = match.group(0)
        lines = body.count("\n") + (0 if body.endswith("\n") else 1)
        if prose_lines(body) < MIN_LINES:
            continue
        if "~english" in body and "~spanish" in body:
            continue
        if LICENCE.search(body):
            continue
        yield text.count("\n", 0, match.start()) + 1, lines


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    findings = []
    for sub in ROOTS:
        for base, _dirs, names in os.walk(os.path.join(root, sub)):
            for name in sorted(names):
                if not name.endswith(SUFFIXES):
                    continue
                path = os.path.join(base, name)
                with open(path, encoding="utf-8", errors="replace") as fh:
                    text = fh.read()
                for line, size in offending_blocks(text):
                    findings.append((os.path.relpath(path, root), line, size))

    by_file = {}
    for path, _line, size in findings:
        entry = by_file.setdefault(path, [0, 0])
        entry[0] += 1
        entry[1] += size

    print("comment parity: %d blocks of %d lines or more carry only one "
          "language, in %d files" % (len(findings), MIN_LINES, len(by_file)))
    for path in sorted(by_file, key=lambda p: -by_file[p][1]):
        count, lines = by_file[path]
        print("  %5d lines in %3d blocks  %s" % (lines, count, path))

    if len(findings) > CEILING:
        print("\nFAIL: %d blocks, and the ceiling is %d.  This number may come "
              "down and may not go up: a comment that explains something has "
              "to explain it in both languages." % (len(findings), CEILING))
        return 1
    if len(findings) < CEILING:
        print("\nThe ceiling is %d and there are %d.  Lower CEILING in "
              "doc/check_comment_parity.py to %d, in the same commit, or the "
              "ground gained is given back for free."
              % (CEILING, len(findings), len(findings)))
    print("\nOK")
    return 0


if __name__ == "__main__":
    sys.exit(main())

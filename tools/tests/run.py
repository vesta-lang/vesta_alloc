# -*- coding: utf-8 -*-
#
# VestaVM -- Distributed Virtual Machine
#
# Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
# License: MIT (see LICENSE).  Part of the VestaVM family.
"""The tests for `alloc_tree`.  Exits 0 or 1.

    python tools/tests/run.py

Two halves, because the tool has two: the model, in Python, and the page's
scripts, in JavaScript.  The second half runs on a DOM stub under `node`.

WHAT HAPPENS WITHOUT `node`: it is SKIPPED and said, and a skip is counted
apart -- never as a pass.  A suite that reports green because half of it did
not run is worse than one that fails.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, TOOLS)

import test_model                                   # noqa: E402
from alloc_tree.page import _payload                # noqa: E402

FAILURES = [0]
SKIPPED = [0]


def say(ok, what):
    print("  [%s] %s" % ("OK  " if ok else "FAIL", what))
    if not ok:
        FAILURES[0] += 1


def check_sources_are_text():
    """No source file of the tool may contain a NUL byte.

    THIS IS NOT PEDANTRY, it happened.  A separator meant as `\\u0000` went
    into `tree.js` as the character itself.  JavaScript was perfectly happy --
    every test passed, the page worked -- but `grep`, `diff` and `file` all
    decided the source was binary and stopped showing it, which is how a file
    quietly stops being reviewable.
    """
    roots = [os.path.join(TOOLS, "alloc_tree"),
             os.path.join(TOOLS, "alloc_tree", "templates"),
             HERE]
    for root in roots:
        for name in sorted(os.listdir(root)):
            path = os.path.join(root, name)
            if not os.path.isfile(path):
                continue
            if not name.endswith((".py", ".js", ".css", ".j2", ".md", ".txt")):
                continue
            with open(path, "rb") as handle:
                say(b"\0" not in handle.read(),
                    "%s es texto, sin bytes NUL" % name)


def run_js(report, tmp):
    """The page's own scripts, executed."""
    node = shutil.which("node")
    if node is None:
        print("  [SALTA] las pruebas de la pagina: no hay `node` en el PATH")
        SKIPPED[0] += 1
        return

    # The payload, not a generated page: this half is about the browser code,
    # and going through the template would drag Jinja2 into a test that does
    # not need it.
    payload = os.path.join(tmp, "payload.json")
    with open(payload, "w", encoding="utf-8") as handle:
        json.dump(_payload(report), handle)

    templates = os.path.join(TOOLS, "alloc_tree", "templates")
    result = subprocess.run(
        [node, os.path.join(HERE, "test_page.js"), payload, templates],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    sys.stdout.write(result.stdout.decode("utf-8", "replace"))
    if result.returncode != 0:
        FAILURES[0] += 1


def main():
    print("== alloc_tree ==")
    tmp = tempfile.mkdtemp(prefix="alloc_tree_test_")
    try:
        print(" -- los fuentes")
        check_sources_are_text()
        print(" -- el modelo")
        report = test_model.run(tmp, say)
        print(" -- la pagina")
        run_js(report, tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if FAILURES[0]:
        print("%d COMPROBACIONES FALLIDAS" % FAILURES[0])
        return 1
    if SKIPPED[0]:
        print("TODO OK, con %d parte(s) SALTADA(S)" % SKIPPED[0])
        return 0
    print("TODO OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())

# -*- coding: utf-8 -*-
#
# VestaVM -- Distributed Virtual Machine
#
# Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
# License: MIT (see LICENSE).  Part of the VestaVM family.
"""Names after the fact, with `addr2line`.

A stripped build has no symbols and no debug info, so the export comes out with
offsets.  The information is not gone -- it is in the unstripped binary of that
same build -- and `addr2line` reads it.  This turns the offsets into names
without having to run the program again.

THE SAME BUILD, and it matters: two builds of the same source lay the code out
differently, so an offset from one resolved against the other gives a name that
is wrong and looks right.  Nothing here can check that, so it is said rather
than assumed.
"""
import struct
import subprocess

from .report import Frame


def image_base(path):
    """The address the binary was LINKED for.  Zero if it cannot be read.

    Not where it was loaded: `addr2line` speaks in link addresses, and the
    export carries offsets from the module base precisely so the two can be put
    together whatever the loader did with it.
    """
    with open(path, "rb") as handle:
        head = handle.read(0x1000)
    if head[:2] == b"MZ":
        e_lfanew = struct.unpack_from("<i", head, 0x3C)[0]
        opt = e_lfanew + 4 + 20
        magic = struct.unpack_from("<H", head, opt)[0]
        if magic == 0x20B:                      # PE32+
            return struct.unpack_from("<Q", head, opt + 24)[0]
        if magic == 0x10B:                      # PE32
            return struct.unpack_from("<I", head, opt + 28)[0]
        return 0
    if head[:4] == b"\x7fELF":
        ph_off = struct.unpack_from("<Q", head, 0x20)[0]
        ph_ent = struct.unpack_from("<H", head, 0x36)[0]
        ph_num = struct.unpack_from("<H", head, 0x38)[0]
        lowest = None
        for i in range(ph_num):
            at = ph_off + i * ph_ent
            if at + 0x18 > len(head):
                break
            if struct.unpack_from("<I", head, at)[0] != 1:   # PT_LOAD
                continue
            vaddr = struct.unpack_from("<Q", head, at + 0x10)[0]
            if lowest is None or vaddr < lowest:
                lowest = vaddr
        return lowest or 0
    return 0


def resolve_offsets(binary, offsets, tool="addr2line"):
    """`offset -> (function, file, line)` for every offset it can.

    Without `-i`: each address then produces EXACTLY two lines, which is what
    makes a single batch possible.  With inlining the chain would be of
    variable length and there would be no way to tell where one address ends
    and the next begins -- and guessing would hang one function's frames off
    another address.  The chain is what the built-in DWARF reader is for.
    """
    if not offsets:
        return {}
    base = image_base(binary)
    text = "\n".join("0x%x" % (base + off) for off in offsets)
    try:
        done = subprocess.run([tool, "-f", "-C", "-e", binary],
                              input=text, capture_output=True, text=True)
    except OSError as exc:
        raise IOError("could not run %s: %s" % (tool, exc))
    lines = done.stdout.splitlines()
    if len(lines) != 2 * len(offsets):
        raise IOError("%s answered %d lines for %d addresses"
                      % (tool, len(lines), len(offsets)))
    out = {}
    for i, off in enumerate(offsets):
        name = lines[2 * i].strip()
        where = lines[2 * i + 1].strip()
        if name in ("??", ""):
            continue                            # it does not know: leave as is
        path, _, line = where.rpartition(":")
        try:
            line = int(line.split(" ")[0])
        except ValueError:
            line = 0
        out[off] = (name, "" if path in ("??", "") else path, line)
    return out


def add_names(report, binary):
    """Fills in the names the export could not, and says how many.

    Only the ones that are still an offset: whatever the program itself
    resolved is better, because it carries the inlining chain and this does
    not.
    """
    pending = [s.pc for s in report.sites
               if not report.frames.get(s.sid)
               or report.frames[s.sid][0].function.startswith(("fn +0x", "+0x"))]
    found = resolve_offsets(binary, pending)
    for site in report.sites:
        hit = found.get(site.pc)
        if hit is not None:
            report.frames[site.sid] = [Frame(hit[0], hit[1], hit[2], False)]
    return len(found), len(pending)

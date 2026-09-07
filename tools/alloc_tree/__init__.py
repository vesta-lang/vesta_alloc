# -*- coding: utf-8 -*-
#
# VestaVM -- Distributed Virtual Machine
#
# Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
# License: MIT (see LICENSE).  Part of the VestaVM family.
"""Reads what `util::write_alloc_csv` wrote and shows where allocations start.

    report    the five CSVs, loaded and joined, every column kept
    tree      the inlining chains of every site merged into one tree
    text      the whole export written out for a terminal
    symbols   names after the fact, with addr2line, for a stripped build
    page      the whole export as a self-contained page, with Jinja2

Only `page` needs anything installed.  Loading, folding and printing use the
standard library alone, so the terminal view -- which shows the SAME thing, not
less of it -- keeps working where nothing can be added.
"""
from .report import Frame, Report, Site
from .text import print_report, print_table
from .tree import Node, build_tree, human_bytes, print_tree

__all__ = ["Frame", "Report", "Site", "Node", "build_tree", "human_bytes",
           "print_tree", "print_report", "print_table"]

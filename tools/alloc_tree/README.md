# alloc_tree

*[Version en espanol](README.es.md)*

Where the allocations are born, as a tree you can walk.

The allocator can print a report, and that report answers **what allocates
most**. It cannot answer the next question, which is the one that leads
somewhere: *whose* code asked for it, through which path, and what the picture
looks like once a branch is folded away. That is walking and drilling in — a
tool's job, not a printout's.

## Getting the data

The program under study writes the export itself. Two environment variables:

```sh
VESTA_HOST_ALLOC_SITES=1 VESTA_HOST_ALLOC_CSV=/tmp/run  ./your_program
```

`..._SITES` turns the recording on — without it nothing is written down, and
the jump over `operator new` that records call sites is not even installed, so
a program that does not ask pays nothing. `..._CSV` says where to put the
files; the directory is **created**, with every level it is missing.

Six files come out, joined on `site_id`:

| | |
| :--- | :--- |
| `sites.csv` | one row per site: counts, bytes, purpose, shape, address, which size classes |
| `frames.csv` | one row per (site, depth): the inlining chain, and whose module each frame is |
| `summary.csv` | key/value: totals, region, and what could **not** be resolved |
| `sizes.csv` | the histogram of requested sizes, for the whole process |
| `site_sizes.csv` | the same histogram **per site**, sparse |
| `tags.csv` | how much each declared purpose accounts for |

`site_sizes.csv` is what lets a *branch* of the tree answer "many small ones or
a few large ones". The global histogram cannot: it cannot be handed back out to
the sites that formed it, and a site that made a million 32-byte allocations
looks exactly like one that made a single 16 MiB one — same average, same class
mask.

Several files and not one because a site has many frames: a single table would
repeat every site once per frame.

## Looking at it

```sh
python -m alloc_tree /tmp/run
```

Writes a self-contained page and opens it.

The page is a **tree grid**, not a chart: an indented call stack you fold and
unfold, with the numbers in columns next to it — allocations through the frame
and allocations that end *in* it, the same two for bytes, how many sites it
covers, whose module it is, the source file and the offset. Names are printed
in full: a C++ symbol runs long and the tail is where the template arguments
are, which is the part that tells two instantiations apart.

Click a column to sort siblings by it, type in the box to keep only the
branches that contain a match, tick *weigh by bytes* when what matters is size
rather than count. A button switches between the two directions:

- **from the binary inwards** — from the function that exists in the binary,
  through everything the optimiser inlined into it. *Where does this allocation
  come from.*
- **from the allocation outwards** — from where it physically happens. *Who
  ends up calling `operator new` the most* — this is the one that groups all
  the `std::vector` growth together.

Clicking a row opens it up into the raw rows it was folded from, so any number
in the tree can be checked instead of believed.

And under the tree, in their own tabs, are **the five files whole** — every
row, every column, including columns this tool has no code for. Nothing is
summarised away: a tool that shows part of a measurement makes the rest
impossible to look for, and the reader cannot tell which part is missing.

The page carries its data inside and loads nothing, so it still works with no
network, copied off a machine you only reach over ssh, and in five years.

### On a machine where nothing can be installed

```sh
python -m alloc_tree /tmp/run --text
```

The same tree **and the same five tables** on the terminal, using the standard
library alone. This is not a degraded mode kept around out of politeness: the
machine a memory problem shows up on is usually not the one with a browser, and
a terminal mode that quietly showed less would make the difference between the
two impossible to see.

### Options

| | |
| :--- | :--- |
| `--text`, `-t` | the whole export on the terminal, no dependencies |
| `--tree-only` | in `--text`, the tree without the five tables under it |
| `--bottom-up` | start at the other end |
| `--limit PCT` | in `--text`, fold branches under this % away, saying how many. Default 0: print everything |
| `--names BIN` | put names on the offsets of a stripped build (see below) |
| `--out FILE` | where to write the page |
| `--no-open` | write it and do not launch a browser |

## A build with no symbols

A stripped build — `Release` — has no symbol table and no debug information,
so the export comes out with offsets instead of names. It is still useful:
`.pdata` is a *section*, not a symbol table, so `--strip-all` does not take it,
and every site still says which function contains it. The tree groups them
accordingly — measured on this compiler, 480 sites across 255 functions.

To put names on them, point at the unstripped binary **of that same build**:

```sh
python -m alloc_tree /tmp/run --names ./build/vm
```

Same build matters. Two builds of the same source lay the code out
differently, so an offset from one resolved against the other gives a name that
is wrong and looks right. Nothing here can check that.

## Reading it honestly

Whatever could **not** be done is printed first, and shown on the page:

- no symbol resolver installed — the tree carries addresses, not names;
- sites that could not be resolved;
- the snapshot filled up, so there may be more sites;
- allocations that evicted a weaker entry, which makes those counts lower
  bounds;
- a chain that reached the frame limit and may be cut.

None of that is decoration. A tree with no names looks like a program that
allocates from nowhere, and a truncated one looks like a program with fewer
sites — neither is true, and neither is visible unless it is said.

## What is where

| | |
| :--- | :--- |
| `report.py` | the five files, loaded and joined — every column kept, not only the ones used |
| `tree.py` | the chains of every site merged into one tree |
| `text.py` | the whole export written out for a terminal |
| `symbols.py` | names after the fact, with `addr2line` |
| `page.py` | the export as a page |
| `templates/` | `page.html.j2`, `page.css`, `page.js` — the markup, the styling and the grid |
| `__main__.py` | the command line |

Only `page.py` needs anything installed (`../requirements.txt`: Jinja2, and
nothing else).

## Names, and who decides them

The allocator does not know how to demangle, and should not: mangling belongs
to a language and its compiler, and this library is linked by projects that do
not agree on it — C++ mangles one way, Rust another, a language that ships its
own compiler mangles as it pleases, and C does not mangle at all. Whoever links
it installs a name formatter and decides what *readable* means for their own
code. So the names in the CSV are already the ones that project writes; this
tool needs no demangler of its own.

The same goes for the **Module** column. What counts as a module is a property
of a project's tree — here a directory under `src/`, elsewhere a package, a
crate or a bundle — so the export asks and the project answers.

Both hooks, and the export itself, are reachable **from C as well as from
C++** (`util/alloc_csv_c.h`): a program that never touches `operator new` still
allocates through this library and must be able to ask for its own report.
`examples/c_report.c` is the whole thing in C, and is compiled as C precisely
so that header cannot quietly stop being C.

# vesta_alloc

*[Version en espanol](README.es.md)*

A host memory allocator for C and C++: per-thread size-class free lists,
span-backed large allocations, a bump arena for phase-scoped work, and a
purpose-tagging mechanism that answers *what* a program is allocating for.

It replaces the global `operator new` and `operator delete`, and — when asked —
`malloc` and its family as well, so third-party code you did not write and
cannot recompile allocates from it too.

It depends on nothing but the operating system: no third-party libraries, and
no part of the project it was written for.

> **Status.** In development. The interfaces below work and are covered by
> tests, but the library has not had a stable release yet: names may still
> change, and no performance figures are published because they would describe
> a moving target. `bench/` reproduces them on your machine, which is the only
> place they mean anything.

## Quick start

```cpp
#include "util/alloc/host_allocator.h"

int main() {
    // Nothing to initialise: `new` and `delete` already come here.
    auto *p = new int[1024];
    delete[] p;

    // Or the explicit interface, which C can use as well.
    void *raw = util::host_alloc(64);
    util::host_free(raw);
}
```

From C:

```c
#include "util/alloc/host_allocator_c.h"

void *p = vesta_host_alloc(64);
vesta_host_free(p);
```

## Installing

As part of another CMake project:

```cmake
add_subdirectory(path/to/vesta_alloc)
target_link_libraries(your_target PRIVATE vesta_alloc)
```

On its own:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

A standalone build also produces `libvesta_alloc.a`, the examples and the
benchmarks. `-DVESTA_ALLOC_BUILD_SHARED=ON` adds the dynamic library; read the
two notes below before using either.

### Linking the static archive

**Read this one.** From a static archive the linker only extracts objects that
something references *by name*, and nothing references `operator new` by name —
the compiler generates the call. The object that defines it is never pulled in,
the program silently keeps the system allocator, and **nothing fails**: the
result is a working binary that is slower, which is the worst failure mode
there is.

The default target is therefore an **object library**, whose objects always go
in. If you link the archive instead, force it:

| toolchain | flag |
| :--- | :--- |
| GNU ld, lld | `-Wl,--whole-archive libvesta_alloc.a -Wl,--no-whole-archive` |
| MSVC | `/WHOLEARCHIVE:vesta_alloc.lib` |
| Apple ld | `-force_load libvesta_alloc.a` |

To confirm it took effect, call `util::host_alloc_active()`, or run with
`VESTA_HOST_ALLOC_STATS=1` and check that the exit summary reports any
allocations at all.

### The shared library

An executable brings its own `operator new` and will not give it up for a
library loaded later, so **the replacement is not reliable from a shared
object**. The C interface (`vesta_host_alloc` and friends) works either way.
That is why the shared build is off by default.

## The allocator

| | |
| :--- | :--- |
| `util::host_alloc(n)` | Allocate. Small sizes come from a per-thread free list. |
| `util::host_alloc_zeroed(n)` | The same, zeroed — without writing memory the system already zeroed. |
| `util::host_alloc_aligned(n, a)` | Over-aligned. Released with `host_free_aligned`. |
| `util::host_realloc(p, n)` | Grows in place where the shape allows it. |
| `util::host_free(p)` | Free, from any thread — including one that did not allocate. |
| `util::host_usable_size(p)` | How much of the block is actually usable. |
| `util::host_alloc_stats()` | Counters: allocations, bytes, chunks, the size split. |

Requests up to a few kibibytes are served from per-thread lists with no
synchronisation at all. Larger ones come from spans — runs of contiguous chunks
that split and coalesce — and above that from the operating system directly.

A block is recognised as ours by two comparisons against the region bounds: no
table, no lock, and nothing added to the free path.

`util::ScratchArena` is the other shape: a bump arena for memory that all dies
together. It does not recycle, which is exactly why it is fast, and it is
documented as unsafe to share between threads on purpose — the caller
guarantees exclusivity instead of paying for a lock on every allocation.

### What it costs, measured

Nanoseconds per operation on one Raptor Lake P-core, against the allocator this
library replaced. `ours` is the best of the three ways in; the full tables,
including the other two and what each committed, are in
[`bench/baseline/`](bench/baseline/).

| | Windows, vs msvcrt | | Linux, vs glibc | |
| :--- | ---: | ---: | ---: | ---: |
| | **ours** | system | **ours** | system |
| `malloc`+`free`, 64 B | **1.33** | 11.92 | **1.26** | 1.72 |
| `malloc`+`free`, 4 KiB | **1.34** | 11.73 | **1.25** | 6.02 |
| `malloc`+`free`, 1 MiB | **3.80** | 3527.90 | **2.19** | 6.42 |
| burst of 512, 64 B | **1.46** | 17.56 | **1.40** | 3.23 |
| churn, 1024 live, 64 B | **1.15** | 13.15 | **1.08** | 2.04 |
| `realloc` 64 B → 1 MiB | **23.77** | 8023.00 | **22.65** | 44.59 |
| `calloc` 8 MiB, read whole | **169532** | 921714 | 170670 | 170657 |
| `calloc` 8 MiB, read 1/64 | 82707 | **25149** | 85720 | 85742 |

**Read the two systems apart, never averaged.** The rival is not the same one:
msvcrt takes about 12 ns to hand back a small block and glibc's `tcache` takes
1.7. So the ~9x on Windows is mostly msvcrt being slow, while the ~1.4x on Linux
is the honest measure of what this design buys over a good allocator. We are the
same speed on both — around 1.3 ns — and that is the number to watch.

**The last row is a real loss and is left in.** A large `calloc` whose caller
barely reads it wins by deferring: the system maps pages the kernel already
holds zeroed and pays only for the ones touched, while this allocator zeroes up
front because its region is committed once and reused, so a recycled block
carries the previous tenant's bytes. The row above is the same request with the
caller reading all of it, where paying up front wins 5.4x. No threshold on
*size* can choose between them — the size is identical; only the caller's
behaviour differs. See `doc/PLAN_RESERVAS.md`.

Every row is the mean of the clean half of eleven interleaved repeats, and the
benchmark measures its own floor first — the same allocator in every column,
where the true ratio is 1.00x — so a row that cannot beat that floor says "too
close" instead of naming a winner. Reproduce with:

```bash
./vesta_alloc_bench_vs_malloc       # head to head, per call
./vesta_alloc_bench_allocator       # the same, per pattern, plus threads
```

## Purpose tags

An arena serves an allocation far more cheaply than a general allocator, but
only for allocations that die soon and never grow. Getting that wrong is
expensive and quiet: growing containers abandon their old buffers, and an arena
never reclaims them.

So a purpose has two axes, and `0` means *unknown* on both:

```cpp
util::AllocScope phase{{util::AllocUse::Medium, util::AllocShape::Growing}};
// everything this thread allocates until the scope ends is counted there,
// including std::string and std::vector, which cannot declare anything
```

`unknown` being the default is not decoration: the per-thread state is a POD
zeroed at start-up, so it costs no initialisation, and it is honest — whatever
the report shows as unknown is literally what has not been classified yet.

A scope applies to **its own thread**. If work is farmed out, read the tag on
the dispatching thread and re-apply it on the worker; `examples/purpose_tags.cpp`
shows the pattern.

## Replacing `malloc`

`operator new` is replaced, so every `new` in the program already arrives here.
The C entry points had nothing equivalent, and that gap was never only C's: a
`.cpp` calling `malloc` directly went to the system just the same.

So `malloc`, `calloc`, `realloc` and `free` are redirected **at link time**,
with `-Wl,--wrap=`, and the flags ride on the target as `INTERFACE` — anything
that links `vesta_alloc` inherits them without knowing they exist. Two
properties follow from renaming at the final link rather than per library:

- **It reaches every object in the link**, wherever it came from. Third-party
  code you did not write is covered without touching a line of it and without
  checking whether the version you have happens to expose an allocator hook.
- **The language of the caller is irrelevant.** A `.c` and a `.cpp` calling
  `malloc` are the same undefined reference by the time the linker sees them.

A linker without `--wrap` (Apple's, Microsoft's) is reported at configure time
rather than producing a link line that fails on an unknown option.

Foreign blocks are handled rather than assumed away: by the time this is in
force the C runtime has already allocated during start-up, and anything it
hands back for the caller to release arrives as a block this allocator never
made. The interposed `free` recognises that and returns it to the real one. The
rule inside the allocator does not move — `host_free` still treats a foreign
pointer as the hard error it is.

### Aligned entries

`posix_memalign`, `aligned_alloc`, `memalign` and, on Windows, the whole
`_aligned_*` family go through the same mechanism. An aligned allocation is an
allocation, and leaving it out would produce a report missing exactly the
memory of the types that ask for a cache line or a page.

On POSIX such a block is released with plain `free`, so it has to be
recognisable by itself: it is served as a span, whose header `free` already
finds by masking. No marker, no side table, and nothing added to the free path.

### Reaching inside the C runtime

What the C library allocates *inside itself* and hands back — `strdup`,
`getline`, `asprintf` — never becomes a pending reference, so no linker can
rename it. Closing that needs a different mechanism on each platform, and both
are on by default:

| | |
| :--- | :--- |
| **ELF** | `VESTA_ALLOC_DEFINE_MALLOC` — define the symbol. A definition in the executable outranks the C library's, and its own calls go out through the PLT. |
| **Windows** | `VESTA_ALLOC_HOOK_MSVCRT` — write a jump at the entry of `msvcrt!malloc`. A DLL has no PLT and an internal call never leaves it, so the code is the only place left. Only the C runtime is touched. |

Neither can coexist with `--wrap` for the same symbol: with both in force the
linker resolves `__real_malloc` against the only definition there is, and the
first allocation calls itself. The build removes the renaming for exactly the
symbols the other mechanism defines.

Both can be turned off, and that is not politeness: without something to
compare against there is no way to tell whether they buy anything.

## Allocation reports

With recording on, the allocator keeps who allocated, how much, and what for —
in a per-thread table, without locks and without allocating to do it.

```cpp
util::AllocSite sites[64];
unsigned n = util::alloc_sites_snapshot(sites, 64);
```

### Names, files and modules

An offset is not an answer. The library reads its own debug information and its
own symbol table, so a report comes out with names without linking any
symbolising library:

```cpp
vesta_alloc_set_symbol_resolver(vesta_self_resolver);
vesta_alloc_write_csv("report/");
```

Two lines, and the same two from C — `examples/symbol_report.cpp` and
`examples/c_symbol_report.c` are the same program written twice, because "the
library can name its own addresses" would be worth half if it were a C++
capability.

The resolver tries three things in order, and each says what it could not do
rather than inventing:

| | gives |
| :--- | :--- |
| Debug information (DWARF) | the whole inlining chain: function, file, line |
| The symbol table | one frame, a name, no file |
| Function ranges (`.pdata`, `st_size`) | not a name — where the function starts, which still groups the sites of one function together |

It asks **whose** address it is first. All three read the program's own image,
so an address belonging to a system library would otherwise come back wearing a
name from our table — and a wrong name is worse than a missing one, because the
missing one asks a question and the wrong one closes it. For a foreign module
the symbols are read from its file, not only from what it exports.

### The report as data

`vesta_alloc_write_csv` writes six CSV files, and `tools/alloc_tree` turns them
into a page: a sortable tree that folds by call stack, module, file or purpose,
with the size split per site, two interface languages, and filters for scope
("only my own calls") and language (C or C++). Everything measured always
travels in the page — the filtering happens there, because deciding what to
look at is a way of looking and not a way of exporting.

## Memory primitives

`memcpy` and `memset` are not asked of the C library. They were the last two
unresolved symbols: without them the allocator runs where there is no libc. The
second reason is small sizes, which in an allocator are the common case.

The layout is the point: **adding a new architecture means creating a folder
and one branch in the dispatcher**, touching nothing else.

```text
util/mem/vesta_memcpy.h      the only thing included from outside
util/mem/vesta_memset.h
util/mem/mem_config.h        what is compiled in, and why
util/mem/mem_inline.h        small sizes: no ISA, no loop, no call
util/mem/x86/                one file per micro-ISA (SSE2, AVX2, ERMS)
util/mem/generic/            where there is no folder of its own yet
```

Dispatch goes cheapest first: overlapping blocks with no loop at the smallest
sizes, then a fixed number of moves addressed from both ends, then a vector
loop with the destination aligned before entering, then `rep movsb` where the
microcode does the work.

There are two entry points per operation, and the difference is whether a call
can happen:

| | |
| :--- | :--- |
| `vesta_memcpy`, `vesta_memset` | Dispatch on CPU. One call, which pays for itself above a threshold. |
| `..._inline` | **Never call anybody.** They stay on the base path, so the compiler can expand them — a function compiled for a wider ISA cannot be inlined into one that is not. |
| `..._noinline` | One call and nothing else. At large sizes the inline expansion is hundreds of instructions at every call site. |

They are **C headers, not C++ ones**: for a C dependency to avoid paying a
call, its compiler has to see the body. In C++ they are also available as
`util::vesta_memcpy` and friends, and as typed helpers that take an object
instead of a byte count:

```cpp
util::vesta_memcopy(&dst, &src);          // ONE object
util::vesta_memcopy(v_dst, v_src, count); // `count` objects
util::vesta_memfill(&header, 0);
```

`examples/c_mem_ops.c` is compiled **as C**, which is what keeps the headers
honest about it.

## Configuration

### CMake options

| | default | |
| :--- | :--- | :--- |
| `VESTA_ALLOC_INTERPOSE_MALLOC` | on | `malloc` and friends are this allocator, via `-Wl,--wrap`. |
| `VESTA_ALLOC_DEFINE_MALLOC` | on (ELF) | Also reach what the C library allocates inside itself. |
| `VESTA_ALLOC_HOOK_MSVCRT` | on (Windows) | The same, by patching the C runtime's entry points. |
| `VESTA_ALLOC_BUILD_SHARED` | off | Build the dynamic library too. See the note above. |
| `VESTA_ALLOC_SIZE_HISTOGRAM` | on | Compile in the per-size split. |
| `VESTA_ALLOC_SPAN_CACHE_SLOTS` | 32 | How many span sizes a thread keeps to itself. |
| `VESTA_ALLOC_SPAN_CACHE_BYTES` | 2 MiB | And how much memory that may hold at most. |

### Environment variables

| | |
| :--- | :--- |
| `VESTA_HOST_ALLOC_STATS=1` | Print a summary at exit: counts, committed bytes, the size split and the purpose split. |
| `VESTA_HOST_ALLOC_SITES=1` | Also record where each allocation came from. Implies `..._STATS`. This is what installs the jump over `operator new`; without it those entry points are untouched. |
| `VESTA_HOST_ALLOC_CSV=<dir>` | Write the report as CSV into that directory. |

Set, non-empty and not `0` means on.

Each is read **once, on the first allocation**, and read from the environment
block the operating system gave the process — not through `getenv`, which reads
a copy the C runtime builds while it starts up. The first allocation can happen
before that copy exists: any global whose constructor asks for memory gets
there before `main`. Reading the system's block means there is no "too early",
and it is also why this library needs nothing from the C runtime to answer.

Because the answer is taken before anything is allocated, setting one of these
from inside the program later has no effect. Switching measurement on halfway
through would silently leave every start-up allocation out of the report.

## Thread safety

Every function documents which of three it is, because the distinction is the
design:

- **Safe** — call it from anywhere.
- **Safe by partition** — no synchronisation *because* each thread only touches
  its own data. `host_alloc` and `host_free` are this: the fast path
  synchronises nothing at all.
- **Not safe, deliberately** — the free-list primitives and everything on
  `ScratchArena`. Allocating there is two loads and a store; a lock would cost
  more than the work it protects, so the caller guarantees exclusivity.

Freeing from a thread that did not allocate is fully supported: the block goes
to a lock-free stack owned by the allocating thread, which picks up the whole
stack in one exchange when it next runs dry.

## Platform support

| | |
| :--- | :--- |
| Linux, x86-64 | GCC and Clang. Tested. |
| Windows, x86-64 | MinGW (GCC). Tested. |
| macOS | Not supported: its linker has no `--wrap`, and the allocator has not been built there. |
| Other architectures | The memory primitives fall back to the generic path; nothing else is architecture-specific. |

C++17 for the library; the C interface is C99.

## Limitations

- **No release has been made.** Names and layout may still change.
- **The shared library cannot replace `operator new` reliably** — see above.
- **macOS is not supported.**
- **A report needs debug information to name things.** Without it a site comes
  out as the offset where its function starts, which still groups the sites of
  one function together but does not name them.
- **The allocator does not return memory to the system.** Freed chunks are
  reused, not unmapped, so peak memory is the peak the program reached.
- Where it loses to a good system allocator, and why, is reproduced by
  `bench_vs_malloc`, which prints the cases it loses as well
  as the ones it wins.

## License

**MIT** — see `LICENSE`. Use it, ship it, change it, sell it; just keep the
copyright notice.

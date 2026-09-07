# vesta_alloc

*[Version en espanol](README.es.md)*

A host memory allocator: size-class free lists per thread, span-backed large
allocations, a bump arena for phase-scoped work, and a purpose-tagging
mechanism so you can find out *what* your program is actually allocating for.

Written for the VestaVM compiler, where `malloc`/`free` were **18.5% of compile
time** and spread across every call site -- the largest single site was 8.7% of
that, so no local fix could move it. Replacing the allocator moves all of them
at once.

It depends on nothing but the operating system. No third-party libraries, no
part of the compiler it came from.

```
     this allocator      3.2 ns/op
     system malloc      45.4 ns/op          ~14x, measured by tests/test_host_allocator
```

## What is in here

| | |
| :--- | :--- |
| `include/util/host_allocator.h` | The allocator. Replaces global `operator new`/`delete`. |
| `include/util/host_allocator_c.h` | The same thing with C linkage, for C libraries and their allocator hooks. |
| `include/util/host_allocator_layout.h` | The geometry: region, chunks, size classes. Shared by everything below. |
| `include/util/alloc_tag.h` | Two-axis purpose tag (lifetime x fixed-or-growing). |
| `include/util/scratch_arena.h` | Bump arena for memory that all dies together. |
| `include/util/os_memory.h` | The only place that talks to the OS about memory. Reserve and commit are separate. |
| `include/util/thread_slot.h` | A per-thread pointer that does not go through emulated TLS. |
| `include/util/vesta_memcpy.h` | Copy (and overlap-tolerant move) without calling the C library. |
| `include/util/vesta_memset.h` | Fill, likewise. |
| `include/util/mem/` | The implementations: **one folder per architecture, one file per micro-ISA**.  See below. |

### The memory primitives

`memcpy` and `memset` are not asked of the C library.  The strong reason is
that they were the **last two unresolved symbols**: without them the allocator
runs where there is no libc.  The second is small sizes, which in an allocator
are the common case: under 16 bytes this calls nobody -- overlapping blocks, no
loop -- and wins there by 2x to 4.5x.

They are laid out like this, and the layout is the point: **adding NEON means
creating `mem/arm/` and one branch in the dispatcher**, touching nothing in the
x86 code.

```
util/vesta_memcpy.h          <- the only thing included from outside
util/vesta_memset.h
util/mem/mem_config.h        what is compiled in, and why
util/mem/mem_inline.h        under 16 bytes: no ISA, no loop, no call
util/mem/x86/x86_vec.h       vector types
util/mem/x86/x86_cpuid.h     CPUID and XGETBV, wrapped and nothing else
util/mem/x86/x86_cpu.h       what this CPU can do (it reads the above)
util/mem/x86/sse2_memcpy.h   base path, the only one that can be inlined
util/mem/x86/sse2_memset.h
util/mem/x86/avx2_memcpy.h   only if the CPU has it
util/mem/x86/avx2_memset.h
util/mem/x86/erms_memcpy.h   `rep movsb`: microcode does the work
util/mem/x86/erms_memset.h
util/mem/generic/scalar_*.h  where there is no folder of its own yet
```

Dispatch goes cheapest first:

| size | what it does |
| :--- | :--- |
| < 16 B | overlapping blocks: no loop, no call |
| 16 - 128 B | up to eight moves addressed from both ends, **no loop** |
| 128 B - 2 KiB | vector loop, **with the destination aligned before entering** |
| > 2 KiB | `rep movsb` / `rep stosb`, microcode does the work |

**The thresholds come from measurement**, and each one carries its table in the
file where it lives; none of them was copied from anybody.

Two of those decisions came not from a benchmark but from **disassembling
glibc**, and are worth calling out: that there is no loop below 128 bytes, and
that the loop aligns the destination before starting.  The second is the bigger
one -- an unaligned store that crosses a cache line is split in two, and in a
loop you pay that every iteration --: on a 1 KiB copy it is 9.6 ns against 5.2.

There are **two entry points per operation**, and the difference is whether a
call can happen:

| | |
| :--- | :--- |
| `vesta_memcpy` / `vesta_memset` | Dispatch on CPU.  On a machine with AVX2 they pay one call, which pays off from 32 bytes up. |
| `vesta_memcpy_inline` / `vesta_memset_inline` | **Never call anybody.**  They stay on the base path -- a function with `target("avx2")` cannot be inlined into one without it -- and with a constant size the compiler expands it. |
| `vesta_memcpy_noinline` / `vesta_memset_noinline` | One call and that's it.  At large sizes the inline expansion is hundreds of instructions AT EVERY CALL SITE; here the call site is tiny.  They exist so you can choose, not to replace anything. |

### And in C++, the typed version

```cpp
util::vesta_memcopy(&dst, &src);          // ONE object
util::vesta_memcopy(v_dst, v_src, count); // `count` OBJECTS
util::vesta_memfill(&header, 0);
```

`memcpy`/`memset` count **bytes**; `memcopy`/`memfill` count **objects**.  The
names differ on purpose: with the same name, template deduction would pick the
typed one without anybody writing it, and the third argument would silently
change unit.

What the type buys: `sizeof(T)` makes the length constant and `alignof(T)`
**removes the prologue that aligns the destination**.  That prologue computes an
offset at run time, which turns a constant length into a variable one and stops
the loop from being unrolled.  Disassembled, a 256-byte fill goes from 66
instructions with 4 branches to **19, straight line**.

It shows up with sizes that are NOT a multiple of the store width, which is
where the prologue is paid in full.  At round sizes the cascade already folds on
its own and the two tie, which is the honest result rather than a disappointing
one.  And when the type declares no alignment, the typed version invents
nothing: it comes out level with the C one, which is exactly what should happen.

**The timings are not printed here on purpose.**  A table of nanoseconds in a
README is out of date the day after it is written, and it cannot say which
machine, which core, or which compiler produced it -- all three of which change
these numbers more than the code does.  The measurements live in
[`bench/baseline/`](bench/baseline/), one file per toolchain, with the CPU and
the run conditions alongside them, and `bench_memcpy` / `bench_memset` reproduce
them.  That folder also records what is still **wrong**: seven rows where the
typed layer loses to the C one, which by construction it should not.

**These are C headers, not C++ ones.**  For a C dependency to avoid paying a
call, its compiler has to see the body; a C++ layer behind a C wrapper would
hand it exactly the cost being removed.  In C++ they are also available as
`util::vesta_memcpy` and friends.  `examples/c_mem_ops.c` is compiled **as C**
and is what keeps that true.

What each path costs, measured against libc: `bench_memcpy` and `bench_memset`.

## Building

As part of another CMake project:

```cmake
add_subdirectory(path/to/vesta_alloc)
target_link_libraries(your_target PRIVATE vesta_alloc)
```

On its own:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build          # 5 test binaries
./build/vesta_alloc_bench_allocator          # this allocator vs the system one
./build/vesta_alloc_bench_reserve_cost       # what reserving address space costs here
```

Standalone builds produce `libvesta_alloc.a`, the tests, three examples and two
benchmarks. `-DVESTA_ALLOC_BUILD_SHARED=ON` also produces the dynamic library --
read the warning below before you use it.

## The one thing that will bite you

**This library replaces global `operator new` and `operator delete`.** From a
static archive (`.a`, `.lib`) the linker only extracts objects that something
references *by name* -- and nothing references `operator new` by name, because
the compiler generates the call. So the object that defines it is never pulled
in, your program silently keeps the system allocator, and **nothing fails**. You
get a working binary that is slower, which is the worst possible failure mode.

The default target is therefore an **object library**, whose objects always go
in. If you link the static archive instead, force it:

```
GNU ld / lld    -Wl,--whole-archive libvesta_alloc.a -Wl,--no-whole-archive
MSVC            /WHOLEARCHIVE:vesta_alloc.lib
Apple ld        -force_load libvesta_alloc.a
```

To check it actually took effect, call `util::host_alloc_active()`, or run with
`VESTA_HOST_ALLOC_STATS=1` and see whether the summary at exit reports any
allocations at all.

**The shared library is different.** An executable brings its own `operator new`
and will not give it up for a DLL or `.so` loaded later, so the replacement is
not reliable there. The C interface (`vesta_host_alloc` and friends) works fine
either way. That is why the shared build is off by default.

## Where it loses, and why

Run `vesta_alloc_bench_vs_malloc` and you get a table with rows marked
`<- system wins`. They are there on purpose: an allocator that only publishes
the cases it wins is not telling you anything. On Linux against glibc:

| case | ours | glibc | why |
| :--- | ---: | ---: | :--- |
| `hot` of 1 MiB | ~14 ns | ~12 ns | Span path: one lock and a free-list walk against glibc's cached mmap. |
| `churn` of 64 KiB | ~11 ns | ~10 ns | Same, and it **swings either way between runs** -- it read 1.32x in our favour minutes before it read 0.91x against. At this size the measurement is noisier than the difference. |
| `calloc` of 64 KiB and up | ~430 ns / ~7 us | the same | A tie by construction: at that size the cost is materialising pages, which is identical for both. Neither allocator can be faster at it. |

Everything else we win, from 1.0x to 36x. Three things closed most of the gap,
and each is written up where it lives:

- **Size classes now reach 16 KiB.** They stopped at 2 KiB, so a 4 KiB request
  took a whole 64 KiB chunk -- 16x the waste -- and went through the span path
  with a lock. `hot` of 4 KiB went 6.17 -> 1.63 ns, `churn` 10.8 -> 3.66, for
  +6% peak memory.
- **Spans split and coalesce.** Exact-fit lists meant a 17-chunk span could not
  serve a 1-chunk request, so a growing buffer consumed fresh region every
  round. `realloc` growing to 1 MiB went 3,583 -> 74 ns and peak memory 144 ->
  21 MiB.
- **The thread slot reads the thread pointer with one instruction.** It used to
  call `pthread_getspecific` on every allocation, which is why the small sizes
  lost at 0.83-0.98x.

On Windows the picture is different: we win everywhere by 2x to 500x, because
msvcrt's allocator is much weaker. The `<- system wins` rows above are a Linux
result, and glibc is a strong opponent.

**What is NOT a reason.** None of these is measurement noise, and none is
explained away by "the benchmark is unfair". Two of them are real design gaps
with a known fix, written down above.

## Thread safety

Every function is documented with a `@par Hilos` section saying which of three
it is, because the distinction is the whole design:

- **Safe** -- call it from anywhere.
- **Safe by partition** -- no synchronisation *because* each thread only touches
  its own data. `host_alloc` and `host_free` are this: the fast path
  synchronises nothing at all.
- **Not safe, deliberately** -- `pop_block`, `push_block`, and everything on
  `ScratchArena`. Allocating is two loads and a store; a lock there would cost
  more than the work it protects. The caller guarantees exclusivity instead.

Freeing from a thread that did not allocate is fully supported: the block goes
to a lock-free stack owned by the allocating thread, which picks up the whole
stack in one exchange when it next runs dry.

## Environment variables

| | |
| :--- | :--- |
| `VESTA_NO_HOST_SLAB=1` | Turn the allocator off; everything goes to the system. This exists so there is something to compare against -- without it there is no way to know whether an allocator improves anything. |
| `VESTA_HOST_ALLOC_STATS=1` | Print a summary at exit: counts, committed bytes, the breakdown by requested size, and the breakdown by purpose. |
| `VESTA_HOST_ALLOC_SITES=1` | Also record WHERE each allocation came from, and print the sites at exit. Implies `..._STATS`. This is the one that installs the jump over `operator new`; without it those entry points are untouched and cost nothing. |

Set, non-empty and not `0` means on.

Each is read **once, on the first allocation**, and read from the environment
block the operating system gave the process -- the PEB on Windows, `environ` on
POSIX -- not through `getenv`. `getenv` reads a copy the C runtime builds while
it starts up, and the first allocation can happen before that copy exists: any
global whose constructor asks for memory gets there before `main`. Reading the
system's block instead means there is no "too early", and it is also why this
library needs nothing from the C runtime to answer the question.

Because the answer is taken once and before anything is allocated, setting one
of these from inside the program later has no effect -- deliberately. Switching
measurement on halfway through would silently leave every allocation made
during start-up out of the report.

## Purpose tags

A bump arena serves an allocation five times faster than a general allocator,
but only for allocations that die soon and never grow. Getting that wrong is
expensive and quiet: putting a range analysis into an arena -- where the
lifetimes fitted perfectly -- took peak memory from 2,414 MB to 5,455 MB
(**+126%**) for 4% speed, because growing containers abandon their old buffers
and an arena never reclaims them.

So the tag has two axes, and `0` means *unknown* on both:

```cpp
util::AllocScope phase{{util::AllocUse::Medium, util::AllocShape::Growing}};
// everything allocated in this thread until the scope ends is counted there,
// including std::string and std::vector, which cannot declare anything
```

`0` meaning unknown is not decoration: the per-thread state is a POD zeroed at
startup, so the default costs no initialisation *and* it is honest -- it says "I
do not know" rather than guessing. Whatever the report shows as `unknown` is,
literally, the list of what has not been classified yet.

A scope applies to **its own thread**. If work is farmed out, read the tag on
the dispatching thread and re-apply it on the worker; `examples/purpose_tags.cpp`
shows the pattern.

## Using it from C libraries

Most C libraries let you replace their allocator, which is how you get their
memory into the same counters as yours:

```c
cs_opt_mem mem = { vesta_host_alloc, vesta_host_calloc,
                   vesta_host_realloc, vesta_host_free, vsnprintf };
cs_option(handle, CS_OPT_MEM, (size_t)&mem);
```

The same shape works for `sqlite3_config(SQLITE_CONFIG_MALLOC, ...)`,
`CRYPTO_set_mem_functions` and zlib-style `zalloc`/`zfree` hooks. Check what the
version you actually vendored exposes rather than assuming.

`examples/c_basic.c` and `examples/c_library_hook.c` are compiled **as C**, not
as C++. That is deliberate: they are the only thing that checks
`host_allocator_c.h` really is valid C, rather than merely claiming to be.

**One gotcha when calling from a C program**: the library itself is C++, so the
final link needs the C++ standard library. If your executable has only C
sources, your build system will pick the C linker and you will get undefined
references to `std::` symbols. In CMake:

```cmake
set_target_properties(your_c_program PROPERTIES LINKER_LANGUAGE CXX)
```

or link with `g++`/`clang++` instead of `gcc`/`clang`.

## License

**MIT** -- see `LICENSE`. Use it, ship it, change it, sell it; just keep the
copyright notice.

This is part of the VestaVM family, but licensed more permissively than the
compiler itself, which is GPLv2. That is deliberate: a general-purpose library
under a copyleft licence is not really reusable, because it would drag every
program that links it under the same terms. MIT code goes into a GPLv2 project
without friction, which is exactly what VestaVM does with this.

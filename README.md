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
| 16-64 B, one at a time | ~3.6 ns | ~2.2 ns | glibc's per-thread tcache is a very short path for exactly this. We win on bursts of the same sizes (1.5x), which is the shape that actually shows up. |
| 4 KiB - 64 KiB | ~9.5 ns | ~6.5 ns | **A design gap**: size classes stop at 2 KiB and a chunk is 64 KiB, so a 4 KiB request takes a whole chunk and goes through the span path with a lock. Serving 2 KiB-64 KiB from multi-chunk spans would close it. |
| `calloc` of 16-64 B | ~3.6 ns | ~2.3 ns | Small blocks come off a free list, so they carry the previous tenant's bytes and must be cleared. Nothing to skip. |
| `realloc` growing | ~72 ns | ~48 ns | Was 30x worse until spans learned to split and coalesce; now a growing buffer absorbs its free neighbour instead of copying. The rest is glibc's `mremap`, which we cannot use: it would move the block **out of our region**, and then `in_region` would stop recognising it. |

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

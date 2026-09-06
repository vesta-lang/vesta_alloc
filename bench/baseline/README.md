# Baseline measurements

The numbers this library was at on **2026-09-06**, kept so that later work can
be *subtracted* from them instead of remembered.

Two runs of the benchmark never give identical numbers, so "is this faster?"
cannot be answered by reading two tables side by side -- it is answered by
subtracting them, and subtracting needs the older one written down.  That is all
this folder is.

## What was measured

`bench_memcpy` and `bench_memset` on the same machine, once per toolchain:

| system  | toolchain  | rows (copy / fill) |
| :------ | :--------- | :----------------- |
| Windows | gcc 10.3   | 144 / 167 |
| Windows | clang 22.1 | 144 / 167 |
| Linux   | gcc 15.2   | 144 / 167 |
| Linux   | clang 19.1 | 144 / 167 |

All of them on the `P-cores` pass -- pinned to the performance cores.  This is a
hybrid CPU, and an unpinned run mixes two kinds of core in whatever proportion
the scheduler chose, which is not noise that averages away: it is two different
things measured as one.  See `affinity.h`.

## The three files per run

They share a prefix and join on `run_id`:

| file    | rows | what it holds |
| :------ | :--- | :------------ |
| `_cpu`  | 1    | the machine: microarchitecture, ISA, cache sizes, CPU flags, toolchain |
| `_runs` | 1 per pass | which cores it ran on, and the margin floor measured there |
| `_data` | 1 per table row | the three times, the two ratios, the two spreads, the two verdicts |

Kept apart because they change at different rates.  In one file, the fifteen
columns describing the machine repeated identically on every one of the 144
rows, so the first thing on screen was the part nobody needs to read.

The prefix names the bench, the system, the **toolchain**, the microarchitecture
with the ISA actually used, and the timestamp.  Every one of those changes the
numbers, so two files that differ in any of them are not comparable -- and the
name is what you look at when deciding which two to compare.

## How to compare a later run against this

Run the bench again and subtract on `section` + `label`:

```bash
# from wherever the new run left its files
python - <<'PY'
import csv
old = {(r['section'], r['label']): r for r in csv.DictReader(open('OLD_data.csv'))}
for r in csv.DictReader(open('NEW_data.csv')):
    o = old.get((r['section'], r['label']))
    if not o: continue
    a, b = float(o['cpp_ns']), float(r['cpp_ns'])
    change = b / a
    # Solo cuenta lo que se sale de la dispersion de las dos filas: por debajo
    # de eso no hay diferencia que interpretar, hay ruido con nombre.
    margin = max(float(o['ratio_spread']), float(r['ratio_spread']), 0.02)
    if abs(change - 1.0) > margin:
        print('%-26s %-10s %8.2f -> %8.2f  %.3fx' % (
            r['section'], r['label'], a, b, change))
PY
```

The rule that matters: **a difference smaller than the spread of its own row is
not a difference.**  The bench applies it to its own verdicts; a comparison
against this baseline has to apply it too, or it will report improvements that
are the machine having a busy second.

## What was already wrong here

Two things this baseline is honest about, because they bound what it can be used
for:

- **Seven rows where the typed C++ layer LOSES to the C one**, which by
  construction should not happen -- the typed layer can only remove checks, never
  add work.  Three of them are 1 KiB copies under gcc 15.2 (0.88x, and only that
  toolchain); the rest are fills under clang 22.1, two of them in the *control*
  section, where both columns run the same code.  Open, not explained.
- **The `libc` column measures the C library, not the compiler.**  It is reached
  through an opaque function pointer on purpose: written plainly with a constant
  length, the compiler recognises `memcpy`/`memset` and expands its own code in
  place, so that column was measuring the compiler -- with an advantage the other
  two columns do not get, because it does not recognise them.  Any earlier
  numbers comparing against `libc` are void.  The C-versus-C++ numbers are not
  affected: neither of our columns changed.

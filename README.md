# SentinEL

A security-enforcing C compiler toolchain built on Clang/LLVM 23. `sentinel-cc`
is a drop-in replacement for `clang`. If the C code contains one of the bugs
below, compilation **fails with an error and no binary is produced**.

| Check | Where it runs | Implemented by |
|---|---|---|
| Banned unsafe calls: `gets`, `strcpy`, `strcat`, `sprintf`, `vsprintf`, `scanf`/`sscanf` with bare `"%s"` | Front end (AST) | **Our** Clang plugin `SentinelAST` |
| Double-free | Middle end (LLVM IR, SSA) | **Our** LLVM pass `SentinelIR` |
| Use-after-free (load/store/GEP through a freed pointer) | Middle end (LLVM IR, SSA) | **Our** LLVM pass `SentinelIR` |
| Use of an uninitialised local variable | Front end | **Clang's existing** `-Wuninitialized`, which `sentinel-cc` turns into an error. We did not write this analysis. |

See [docs/HOW_IT_WORKS.md](docs/HOW_IT_WORKS.md), [docs/LIMITATIONS.md](docs/LIMITATIONS.md)
and [docs/SYLLABUS_MAPPING.md](docs/SYLLABUS_MAPPING.md).

## Requirements

- macOS on Apple Silicon, Xcode Command Line Tools (for the SDK headers)
- Homebrew LLVM 23 (`brew install llvm`), at `/opt/homebrew/opt/llvm`
- CMake (`brew install cmake`), Python 3 (ships with the Command Line Tools; used by the benchmark only)

Apple's `/usr/bin/clang` is not used anywhere.

## Build

```bash
cmake -S . -B build
```

```bash
cmake --build build
```

This produces `build/lib/SentinelAST.dylib` (Clang plugin) and
`build/lib/SentinelIR.dylib` (LLVM pass plugin). CMakeLists.txt selects
Homebrew `clang`/`clang++` by default.

## Usage

Use `sentinel-cc` exactly like `clang`:

```bash
./sentinel-cc -O2 -Wall prog.c -o prog
```

To report problems as warnings and still build:

```bash
./sentinel-cc --sentinel-warn-only prog.c -o prog
```

What `sentinel-cc` adds to your arguments:

```
-fplugin=build/lib/SentinelAST.dylib       # our AST check
-fpass-plugin=build/lib/SentinelIR.dylib   # our IR check
-g                                         # so IR findings have file:line:col
-Wuninitialized -Werror=uninitialized      # Clang's own check, as an error
-isysroot $(xcrun --show-sdk-path)         # only if you gave no sysroot/SDKROOT
```

In warn-only mode `-Werror=uninitialized` is left out,
`-fplugin-arg-sentinel-warn-only` is passed to the AST plugin, and the
environment variable `SENTINEL_WARN_ONLY=1` is set for the IR pass.

The plugins can also be used with plain clang:

```bash
/opt/homebrew/opt/llvm/bin/clang -isysroot $(xcrun --show-sdk-path) -g -fplugin=build/lib/SentinelAST.dylib -fpass-plugin=build/lib/SentinelIR.dylib -c prog.c
```

## Demo

The output below was captured from real runs.

```
$ ./sentinel-cc tests/bad/banned_gets_scanf.c -o demo
tests/bad/banned_gets_scanf.c:10:5: error: [SentinEL] call to unsafe function 'gets' (CERT MSC24-C, STR31-C); use fgets(buf, sizeof buf, stdin) instead
tests/bad/banned_gets_scanf.c:11:5: error: [SentinEL] 'scanf' with unbounded "%s" can overflow the buffer (CERT STR31-C); give a width like "%31s" or use fgets
tests/bad/banned_gets_scanf.c:12:5: error: [SentinEL] 'sscanf' with unbounded "%s" can overflow the buffer (CERT STR31-C); give a width like "%31s" or use fgets
1 warning and 3 errors generated.          (exit 1, no ./demo produced)

$ ./sentinel-cc tests/bad/double_free_branch.c -o demo
error: tests/bad/double_free_branch.c:12:5: [SentinEL] double-free in function 'process': pointer may already have been freed (at line 10) (CERT MEM30-C: set the pointer to NULL after free)

$ ./sentinel-cc tests/bad/uaf_one_branch.c -o demo
error: tests/bad/uaf_one_branch.c:18:5: [SentinEL] use-after-free in function 'main': memory accessed through a pointer that may have been freed (at line 14) (CERT MEM30-C: do not access freed memory)

$ ./sentinel-cc tests/bad/uninit_simple.c -o demo
tests/bad/uninit_simple.c:6:9: error: variable 'total' is uninitialized when used here [-Werror,-Wuninitialized]

$ ./sentinel-cc --sentinel-warn-only tests/bad/uaf_simple.c -o demo
warning: tests/bad/uaf_simple.c:16:23: [SentinEL] use-after-free in function 'main': ... [-Wbackend-plugin]
1 warning generated.                       (exit 0, ./demo produced)

$ ./sentinel-cc tests/good/linked_list.c -o demo && ./demo
3
```

(The extra "1 warning" in the first example is the SDK's own deprecation
warning for `gets`. Source-snippet lines are omitted above.)

## Tests

```bash
./run_tests.sh
```

This compiles every file in `tests/bad/` (must be rejected **by the expected
check**) and `tests/good/` (must be accepted with no SentinEL diagnostics). It
prints a table and saves it to `results/test_results.txt`. Current result:
**19 / 19 pass** (11 bad, 8 good). The good programs include tricky cases that
must *not* be flagged: free followed by a new malloc, free on one branch with
use only on the other branch, malloc/free inside a loop, free then `p = NULL`
with a guarded use, list teardown, and `p = realloc(p, n)` plus the
`tmp = realloc(p, n); if (!tmp) free(p);` failure path.

## Benchmark

```bash
./benchmark.py 10
```

This compiles the `tests/good` programs with plain clang, `clang -g`, clang
with our plugins (no wrapper) and `sentinel-cc`, at `-O0` and `-O2`. Each
configuration runs 10 times, and the averages go to `results/benchmark.txt`
and `.csv`. In our run, the checks themselves cost 0.7–2.2% over `clang -g`,
which is within run-to-run noise. Total `sentinel-cc` overhead vs plain clang
was 18–21%, about 4–5 ms per file, and almost all of that is the bash wrapper
starting up. These are tiny programs, so the absolute numbers are small. See
the results file for the exact table.

## Repository layout

```
ast-plugin/SentinelAST.cpp   Clang AST plugin (banned calls)
ir-pass/SentinelIR.cpp       LLVM pass plugin (double-free, use-after-free)
sentinel-cc                  driver script
tests/bad, tests/good        test programs
run_tests.sh, benchmark.py   evaluation scripts
results/                     saved test and benchmark output
docs/                        how it works, limitations, syllabus mapping
```

## macOS notes

- `clang` from Homebrew is dynamically linked against `libclang-cpp.dylib`
  and `libLLVM.dylib`. The plugins therefore do **not** link those libraries;
  they are built with `-undefined dynamic_lookup`, and their LLVM/Clang symbols
  resolve against the copies already loaded in the `clang` process.
- Homebrew clang may not find `stdio.h`, so `sentinel-cc` adds
  `-isysroot $(xcrun --show-sdk-path)` unless you passed a sysroot or set
  `SDKROOT`.

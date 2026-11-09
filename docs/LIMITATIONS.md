# Limitations

SentinEL is a small course project, not a complete verifier. Each item below
was **checked by compiling the example with `sentinel-cc`**. "Missed" means it
compiled with exit status 0, even though the code has the bug.

## Use-after-free / double-free (IR pass)

The pass is **intraprocedural** (one function at a time), **path-insensitive**
(it ignores branch conditions), and only follows pointers that live in **SSA
registers**.

| # | Situation | Example | Result |
|---|---|---|---|
| 1 | **Free inside another function** (interprocedural) | `static void release(char *p){ free(p); }` … `release(p); p[0]='x';` | Missed |
| 2 | **Pointer stored in a struct field** | `struct box b; b.data = malloc(8); free(b.data); b.data[0]='x';` | Missed |
| 3 | **Pointer in a global variable** | `char *g; … free(g); g[0]='x';` | Missed |
| 4 | **Pointer stored in an array** | `ptrs[0] = malloc(8); free(ptrs[0]); ptrs[0][0]='x';` | Missed |
| 5 | **Freed pointer passed to a function** (not a load/store/GEP in this function) | `free(p); puts(p);` | Missed |
| 6 | **`realloc`** (old pointer becomes invalid) | `q = realloc(p,16); p[0]='x';` | **Fixed**: `realloc` / `reallocarray` invalidate their first argument like `free` (`tests/bad/uaf_realloc.c`, `tests/bad/double_free_realloc.c`). `p = realloc(p,n)` and the `if (q == NULL) free(p);` failure path are accepted (`tests/good/realloc_reassign.c`) |
| 7 | **Path-insensitive false positive** | `if (c) free(p); if (!c) { p[0]='x'; free(p); }` | **Wrongly flagged** (both use-after-free and double-free) |
| 8 | **Loop iterations are not counted** | `tests/bad/uaf_in_loop.c`: `if (i == 2) free(arr);` inside a loop | Use-after-free correctly reported; also an extra double-free report, because the analysis doesn't know `i == 2` is true only once |

Why these happen:

- **1 (interprocedural):** each function is analysed separately and a call is
  treated as doing nothing to the freed set. A fix would need function
  summaries ("`release` frees its 1st argument") or inlining before the
  analysis.
- **2–4 (aliasing through memory):** mem2reg only promotes simple local
  scalar variables whose address never escapes. A pointer held in a struct
  field, global or array is re-loaded from memory each time. Every `load`
  produces a new SSA value, and we can't tell it holds the same address
  without **alias analysis**. (Running SROA first would split the local
  struct in case 2 into scalars, but we kept to mem2reg for simplicity.)
- **5:** by design, only `load`, `store` and `getelementptr` count as uses.
  Treating every call argument as a use would flag harmless code such as
  printing the pointer's address.
- **7–8 (path-insensitivity):** at a join we take the union of all incoming
  paths, so the analysis considers paths that can never really happen
  together. That is the price of a simple **may** analysis.

Other limits:

- Only calls to functions literally named `free`, `realloc` and
  `reallocarray` are recognised: no custom allocators or wrappers.
- The "realloc failed, old pointer still valid" case is only recognised when
  the branch tests the `realloc` result directly against `NULL`
  (`if (q == NULL)`, `if (!q)`, `if (q)`). A NULL check that goes through
  another variable or function is not understood, so freeing the old pointer
  there is reported as a double-free.
- Without `-g` the error has no line numbers (it still fails the build).
  `sentinel-cc` always adds `-g`.
- If a file also has an AST error (banned call), clang stops before code
  generation, so the IR pass doesn't run on that file until the AST errors are
  fixed. In `--sentinel-warn-only` mode both are reported.

## Banned functions (AST plugin)

| Situation | Example | Result |
|---|---|---|
| Call through a **function pointer** | `char *(*cp)(char*,const char*) = strcpy; cp(b, argv[1]);` | Missed |
| **Non-literal** `scanf` format | `const char *fmt = "%s"; scanf(fmt, w);` | Missed (we only inspect string literals) |
| Other unbounded formats | `%[...]` without a width, `fscanf`, `vscanf` | Not checked |
| Calls inside **system headers** | macros in SDK headers that call `strcpy` | Ignored on purpose (`unless(isExpansionInSystemHeader())`) |

We don't check that the buffer is actually too small. Every call to a banned
function is rejected, even if it happens to be safe, in line with CERT
guidance to avoid these functions entirely.

## Uninitialised variables (Clang's `-Wuninitialized`)

This is Clang's analysis, not ours, and it has its own limits. For example,
it doesn't look into other functions:

```c
static void maybe_set(int *x, int c) { if (c) *x = 1; }
int v; maybe_set(&v, argc > 1); printf("%d\n", v);   /* missed */
```

## Evaluation limits

- The test suite is 19 small hand-written programs (11 bad, 8 good). It
  shows the checks work on the intended patterns. It isn't a measurement of
  real-world false-positive or false-negative rates.
- The benchmark uses those same small programs, so per-file times are
  dominated by process start-up. Overhead percentages on large files could
  differ.
- Only tested on macOS (Apple Silicon) with Homebrew LLVM 23.1.3.
- C only; C++ was never tried.

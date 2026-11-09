# How SentinEL works

This explains every component in plain language, with the key code, so it
can be defended in a viva.

## 1. The big picture

A C compiler works in phases. Clang/LLVM splits them like this:

```
 prog.c
   │
   ▼
 ┌────────────────────── Clang (front end) ───────────────────────┐
 │ lexer → parser → semantic analysis (Sema) → AST                │
 │                        │                     │                 │
 │      -Wuninitialized ──┘                     ▼                 │
 │   (Clang's own check,          ★ SentinelAST plugin ★          │
 │    we make it an error)        banned-call matcher             │
 │                                             │                  │
 │                                             ▼                  │
 │                                 CodeGen: AST → LLVM IR         │
 └─────────────────────────────────────────────┬──────────────────┘
                                               ▼
 ┌────────────────────── LLVM (middle end) ───────────────────────┐
 │  ★ SentinelIR pass ★  (at "PipelineStart", before any opts)    │
 │  clone module → mem2reg (SSA) → data-flow analysis             │
 │                     │                                          │
 │                     ▼                                          │
 │           optimisation passes (-O0 / -O2 ...)                  │
 └─────────────────────┬──────────────────────────────────────────┘
                       ▼
                 back end → prog.o → linker → prog
```

Both of our components report problems as **compiler errors**. When clang has
an error it deletes the output file and exits with status 1, so the build
system (or the clang driver) never links a binary. That is the "enforcing"
part.

`sentinel-cc` is a bash script that adds the flags that load both plugins
(section 4).

## 2. Front end: the Clang AST plugin (`ast-plugin/SentinelAST.cpp`)

### What an AST is

After parsing, Clang represents the program as an *Abstract Syntax Tree*.
`strcpy(name, argv[1]);` becomes a `CallExpr` node. Its *callee* is a
`FunctionDecl` named `strcpy`, and its children are the argument expressions.
You can see this tree with
`clang -Xclang -ast-dump -fsyntax-only file.c`.

### AST matchers

Clang provides a small pattern language (a DSL) for finding nodes in this
tree. Our pattern is:

```cpp
callExpr(callee(functionDecl(hasAnyName(
             "gets", "strcpy", "strcat", "sprintf", "vsprintf",
             "__builtin___strcpy_chk", "__builtin___strcat_chk",
             "__builtin___sprintf_chk", "__builtin___vsprintf_chk",
             "scanf", "sscanf"))
             .bind("callee")),
         unless(isExpansionInSystemHeader()))
    .bind("call")
```

In words: *any call expression whose callee is a function with one of these
names, and which is not inside a system header*. `.bind("call")` gives the
matched node a name, so the callback can retrieve it.

**Why the `__builtin___..._chk` names?** On macOS, when optimisation is on,
`_FORTIFY_SOURCE` is enabled and the SDK header turns `strcpy(a, b)` into
`__builtin___strcpy_chk(a, b, ...)` through a macro. Without these names, the
check would silently miss bugs at `-O2`. `normalizeName()` maps them back to
`strcpy` for the message.

### The callback and the error

For each match, `BannedCallCallback::run` is called. It looks the name up in a
table of (name, CERT rule, safer alternative):

```cpp
const BannedFunc BannedTable[] = {
    {"gets", "MSC24-C, STR31-C", "fgets(buf, sizeof buf, stdin)"},
    {"strcpy", "STR31-C", "strlcpy(dst, src, sizeof dst) or strncpy + explicit NUL"},
    ...
};
```

and reports through Clang's own diagnostics engine:

```cpp
unsigned ID = DE.getCustomDiagID(Level,
    "[SentinEL] call to unsafe function '%0' (CERT %1); use %2 instead");
DE.Report(Loc, ID) << B->Name << B->Cert << B->Alternative;
```

`Level` is `DiagnosticsEngine::Error` (or `Warning` in warn-only mode).
Because this is a real Clang diagnostic, you get the usual
`file:line:col: error:` format with the source line and a caret.

`scanf`/`sscanf` are only an error when the **format string literal** has a
`%s` with no width. `hasUnboundedPercentS()` scans the string. It skips `%%`
(a literal percent sign) and `%*s` (assignment suppressed, nothing written),
and allows `%31s` (bounded).

**CERT rules used:** STR31-C ("Guarantee that storage for strings has
sufficient space for character data and the null terminator") for all of
them, and additionally MSC24-C ("Do not use deprecated or obsolescent
functions") for `gets`, which was removed in C11.

### How the plugin is plugged in

```cpp
class SentinelASTAction : public PluginASTAction {
  std::unique_ptr<ASTConsumer> CreateASTConsumer(...) override {
    return std::make_unique<SentinelConsumer>(WarnOnly);
  }
  ActionType getActionType() override { return AddBeforeMainAction; }
};
static FrontendPluginRegistry::Add<SentinelASTAction> X("sentinel", "...");
```

- `FrontendPluginRegistry::Add` registers the plugin under the name
  `sentinel` when clang loads the `.dylib` (`-fplugin=`).
- `AddBeforeMainAction` means "run my AST consumer, then continue with the
  normal compilation". The default would only run the plugin when it is
  requested explicitly.
- Arguments come in through `-fplugin-arg-sentinel-<arg>`. We use
  `-fplugin-arg-sentinel-warn-only`.
- `SentinelConsumer::HandleTranslationUnit` runs once the whole file is
  parsed and calls `Finder.matchAST(Ctx)`.

**A bug we hit (good viva story).** Our first version kept the `MatchFinder`
inside the *action* object. Clang destroys the plugin action right after
`CreateASTConsumer` returns, so the consumer used freed memory and clang
crashed. That is a use-after-free in our own code. We confirmed clang's
matcher engine itself worked (using `clang-query`) and fixed the bug by
moving the `MatchFinder` into the consumer, which lives for the whole
compilation.

## 3. Middle end: the LLVM IR pass (`ir-pass/SentinelIR.cpp`)

### LLVM IR, basic blocks, CFG

Clang lowers the AST into LLVM IR, a typed **three-address code**: each
instruction does one operation on at most a few operands and produces at most
one result. Instructions are grouped into **basic blocks**: straight-line code
with one entry and one exit, ending in a branch (`br`) or `ret`. The branches
form the **control-flow graph (CFG)**.

### Step A: SSA form (mem2reg)

At `-O0` Clang does not keep variables in registers. Every local variable gets
a stack slot (`alloca`), and each use is a `load`. This is the real IR for
`tests/good/free_then_reassign.c`, abridged:

```llvm
entry:
  %p = alloca ptr                      ; the variable p lives in memory
  %call = call ptr @malloc(i64 16)
  store ptr %call, ptr %p              ; p = malloc(16)
  ...
if.end:
  %2 = load ptr, ptr %p
  call void @free(ptr %2)              ; free(p)
  %call1 = call ptr @malloc(i64 16)
  store ptr %call1, ptr %p             ; p = malloc(16)   (again)
  ...
if.end4:
  %4 = load ptr, ptr %p                ; is this the freed p?  can't tell
  %arrayidx5 = getelementptr inbounds i8, ptr %4, i64 0
```

Here "the same pointer" is hard to see: every use is a fresh `load` from `%p`.
The **mem2reg** transformation promotes such stack slots to **SSA (Static
Single Assignment)** values. Each value is assigned exactly once, and
**φ (phi) nodes** merge values where control flow joins. After mem2reg (real
output of `opt -passes=mem2reg`):

```llvm
if.end:
  %arrayidx = getelementptr inbounds i8, ptr %call, i64 0
  store i8 97, ptr %arrayidx
  call void @free(ptr %call)              ; frees %call
  %call1 = call ptr @malloc(i64 16)       ; a NEW value %call1
  ...
if.end4:
  %arrayidx5 = getelementptr inbounds i8, ptr %call1, i64 0   ; uses %call1
  store i8 98, ptr %arrayidx5
  call void @free(ptr %call1)
```

The two allocations are now two different values, `%call` and `%call1`. Using
`%call1` after `free(%call)` is clearly fine. **This is why we need SSA**: the
"free then reassign" case is not flagged.

In code:

```cpp
void promoteToSSA(Function &F) {
  std::vector<AllocaInst *> Allocas;
  for (Instruction &I : F.getEntryBlock())
    if (auto *AI = dyn_cast<AllocaInst>(&I))
      if (isAllocaPromotable(AI))      // only simple scalars, address never escapes
        Allocas.push_back(AI);
  DominatorTree DT(F);
  PromoteMemToReg(Allocas, DT);        // the mem2reg algorithm
}
```

`PromoteMemToReg` is the classic SSA construction algorithm. It uses the
**dominator tree** to place φ-nodes at dominance frontiers.

**Two design decisions:**

1. **We analyse a copy.** `CloneModule(M)` makes a private copy, and we run
   mem2reg on the copy only. The user's program is compiled exactly as it would
   be without SentinEL, so an `-O0` debug build still behaves like `-O0`.
2. **No `-disable-O0-optnone` is needed.** At `-O0`, clang marks every
   function `optnone`, and the *pass manager* then skips optional passes on
   such functions. We don't run mem2reg through the pass manager; we call the
   `PromoteMemToReg` utility directly, so `optnone` doesn't apply. Our own pass
   also declares `isRequired() { return true; }` so the pass manager always
   runs it. We tested this: all tests give the same result at `-O0` and `-O2`
   without that flag.

### Step B: the data-flow analysis

**The question:** at each program point, which pointer values **may** have
been freed on *some* path that reaches it?

**The data-flow fact** is a set of SSA pointer values (`FreedSet`). The
analysis is:

- **forward**: information flows from function entry towards the exit;
- **may**: at a join point we take the **union** of what arrives, so "freed
  on *any* incoming path" counts. This is what catches a free on only one
  branch.

**Equations** (B = a basic block, P = its predecessors):

```
IN[B]  = ∪ OUT[P]          for all predecessors P of B
OUT[B] = transfer_B(IN[B])
```

**Transfer function**: walk the instructions of B in order, with S = IN[B]:

| Instruction | Effect on S | Check (final pass only) |
|---|---|---|
| `free(x)` | add `base(x)` (*gen*) | if `base(x)` already in S → **double-free** |
| `load`/`store`/`getelementptr` through `x` | none | if `base(x)` in S → **use-after-free** |
| any instruction `I` that defines a value | remove `I` (*kill*) | – |

`base(x)` is `getUnderlyingObject(x)`. It strips GEPs and casts, so `p`,
`&p[3]` and `p->next` all lead back to the same base `p`.

The *kill* rule is needed for loops. In SSA, an instruction such as
`%line = call @malloc` inside a loop produces a **new** pointer each
iteration. When control comes round again and the instruction re-executes, the
old "freed" fact about `%line` no longer applies. (This is what makes
`tests/good/free_in_loop_fresh.c` pass.)

**φ-nodes:** for `%q = phi [%a, %then], [%b, %else]`, `%q` is added to IN if
the value arriving from some predecessor was freed in that predecessor's OUT.
For example, `%a` might be freed on the `%then` path.

The code:

```cpp
FreedSet computeIn(BasicBlock &BB) {
  FreedSet In;
  for (BasicBlock *Pred : predecessors(&BB)) {      // union over predecessors
    const FreedSet &PO = OutSets[Pred];
    In.insert(PO.begin(), PO.end());
  }
  for (PHINode &Phi : BB.phis()) { ... }            // the phi rule above
  return In;
}

void transfer(BasicBlock &BB, FreedSet &S, bool Report) {
  for (Instruction &I : BB) {
    if (const Value *P = freedPointer(I)) {          // free(P)
      const Value *B = baseOf(P);
      if (Report && S.count(B)) report(I, "double-free", ...);
      S.insert(B);                                   // gen
    } else if (const Value *P = accessedPointer(I)) {// load/store/GEP
      if (Report && S.count(baseOf(P))) report(I, "use-after-free", ...);
    }
    S.erase(&I);                                     // kill
  }
}
```

**Fixpoint iteration:** we repeatedly recompute OUT for every block until
nothing changes. Then one final pass with `Report = true` reports problems
using the stable IN sets. Reporting only in the final pass means each problem
is reported once, with complete information.

**Why does it terminate?** The sets only contain pointer values from this
function, which is a finite set, and they only grow as iteration proceeds
(union and gen are monotone). Each OUT set can only grow a finite number of
times, so the loop stops.

**`realloc`:** `realloc(p, n)` and `reallocarray(p, n, size)` are a "gen"
for `p`, exactly like `free(p)`: on success the old block is released and
only the returned pointer is valid (CERT MEM30-C). The returned pointer is a
new SSA value, so `p = realloc(p, n)` followed by `p[0] = 'x'` is fine. On
failure `realloc` returns `NULL` and leaves `p` alone, so the correct idiom

```c
char *tmp = realloc(p, n);
if (tmp == NULL) { free(p); return 1; }
p = tmp;
```

must not be flagged. To handle it, the set that flows along a CFG edge is
`OUT[pred]`, except when `pred` ends in `br (icmp eq/ne %tmp, null)` and
`%tmp` is a `realloc` result: on the edge where `%tmp` is `NULL`, the old
pointer is removed from the set. This is the analysis's only
branch-sensitive step.

### Worked example: `tests/bad/uaf_one_branch.c`

Real IR after mem2reg (abridged):

```llvm
entry:     %call = call ptr @malloc(i64 64)          ; buf
           br ... label %if.then, label %if.end
if.end:    ... memset(%call, 0, 64) ...
           br i1 %tobool, label %if.else, label %if.then4
if.then4:  call void @free(ptr %call)                ; free(buf) — only on this branch
           br label %if.end5
if.else:   %arrayidx = getelementptr i8, ptr %call, i64 0
           store i8 118, ptr %arrayidx               ; buf[0] = 'v'   (fine)
           br label %if.end5
if.end5:   %arrayidx6 = getelementptr i8, ptr %call, i64 1   ; buf[1] = 'x'
           store i8 120, ptr %arrayidx6
```

| Block | IN | OUT |
|---|---|---|
| entry | {} | {} |
| if.end | {} | {} |
| if.then4 | {} | {%call} |
| if.else | {} | {} — **no report**: the free is not on this path |
| if.end5 | {%call} ∪ {} = **{%call}** | {%call} |

In `if.end5`, the GEP on `%call` finds `%call` in the set, so we report
**use-after-free at line 18 (freed at line 14)**. Union at the join is what
catches the "freed on only one branch" bug.

### Step C: reporting

```cpp
std::string Msg = Loc + ": [SentinEL] use-after-free in function 'main': ...";
F.getContext().emitError(Msg);       // error mode
Ctx.diagnose(DiagnosticInfoGeneric(Msg, DS_Warning));   // warn-only mode
```

`Loc` (`file:line:col`) comes from the instruction's debug location
(`I.getDebugLoc()`), which exists because `sentinel-cc` adds `-g`. Clang
receives the error from LLVM, prints it as `error: ...`, marks the compilation
as failed, deletes the output file and exits with 1.

### Step D: how the pass is loaded

```cpp
extern "C" PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "SentinelIR", "1.0", [](PassBuilder &PB) {
    PB.registerPipelineStartEPCallback(
        [](ModulePassManager &MPM, OptimizationLevel) {
          MPM.addPass(SentinelPass());
        });
  }};
}
```

`-fpass-plugin=SentinelIR.dylib` makes clang call `llvmGetPassPluginInfo`. We
ask to be inserted at the **start** of the optimisation pipeline, which exists
at every `-O` level including `-O0`. That way we see the code before inlining
or other optimisations change it. This is the *new pass manager* plugin
interface (in LLVM 23 the header is `llvm/Plugins/PassPlugin.h`).

Warn-only for the pass is the environment variable `SENTINEL_WARN_ONLY`.
An environment variable was the simplest reliable way to configure a pass
plugin from the driver script; `sentinel-cc` sets it for you.

## 4. The driver: `sentinel-cc`

A ~60-line bash script. It:

1. removes `--sentinel-warn-only` from the arguments (if present);
2. adds `-fplugin=…SentinelAST.dylib -fpass-plugin=…SentinelIR.dylib -g -Wuninitialized`;
3. adds `-Werror=uninitialized` (or, in warn-only mode, the warn-only plugin
   argument and `SENTINEL_WARN_ONLY=1`);
4. adds `-isysroot $(xcrun --show-sdk-path)` if the user gave none, because
   Homebrew clang may not find `stdio.h` otherwise;
5. `exec`s Homebrew clang with everything else unchanged, so `-O2`, `-o`,
   `-c`, `-D...` and linking all work as normal.

### Uninitialised variables: Clang's check, not ours

`-Wuninitialized` is an analysis that **already exists in Clang**. It is a
data-flow analysis over Clang's own source-level CFG. We do not implement it;
`sentinel-cc` only turns it into an error with `-Werror=uninitialized`. In the
viva, present it as *reuse of an existing compiler analysis*, with the
interesting part being that it, too, is a forward data-flow problem.

## 5. Why compilation actually fails

- AST plugin: `DiagnosticsEngine::Report` with level `Error` makes
  `hasErrorOccurred()` true. Clang then skips code generation entirely, so the
  IR pass doesn't run for that file. That is why a file with banned calls shows
  only AST errors. (Verified: a file with both a `strcpy` and a double-free
  reports only the `strcpy` in normal mode, and both in
  `--sentinel-warn-only` mode, where code generation does happen.)
- IR pass: `LLVMContext::emitError` is forwarded to clang's diagnostics as an
  error. Clang finishes, removes the `.o`, and exits with status 1.
- With `-o prog` (compile and link in one step), the clang driver stops after
  the failed compile step, so the linker never runs and `prog` is never
  created. `run_tests.sh` and the demo in the README check this.

## 6. Likely viva questions

- **Why do the analysis on IR instead of the AST?** IR has an explicit CFG
  and SSA form. Loops, `if`, `goto` and `switch` all become simple branches
  between basic blocks, so one data-flow algorithm handles every control
  structure.
- **Why is it a "may" analysis (union)?** We want to catch a bug that happens
  on *some* path, such as a free on only one branch. A "must" analysis
  (intersection) would only catch bugs that happen on *every* path.
- **Is it sound / complete?** Neither. It can miss bugs (false negatives)
  and can flag correct code (false positives). See `LIMITATIONS.md` for the
  concrete cases we tested.
- **Why clone the module?** So the analysis (and the mem2reg it needs) never
  changes the code the user actually gets.
- **What is the cost?** In `results/benchmark.txt` the checks add about 1–2%
  over `clang -g`, which is within noise. Most of `sentinel-cc`'s total
  overhead is the bash wrapper starting up.

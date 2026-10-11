# Syllabus mapping: BCSE307L Compiler Design

How each part of SentinEL relates to the course modules.

| Module | Syllabus topic | Where it appears in SentinEL |
|---|---|---|
| 1 | LLVM and the phases of a compiler | Whole project |
| 3 | Semantic analysis | AST plugin; Clang's `-Wuninitialized` |
| 4 | Intermediate code generation | LLVM IR, which our pass analyses |
| 5 | Basic blocks, flow graphs, data-flow analysis; security checking of VM code | The IR pass's freed-pointer analysis |
| 7 | Static Single Assignment form | mem2reg step inside the IR pass |

## Module 1: LLVM and compiler phases

- SentinEL plugs into **two different phases** of a real production compiler:
  the **front end** (Clang: lexing, parsing, semantic analysis → AST) and the
  **middle end** (LLVM: IR → optimisation passes). The back end (code
  generation, linking) is untouched.
- It shows the LLVM architecture in practice: Clang lowers C to LLVM IR, and
  any number of passes then run over that IR. We add one pass at the
  *PipelineStart* extension point, using the new pass manager
  (`PassBuilder::registerPipelineStartEPCallback`).
- Where: `ast-plugin/SentinelAST.cpp`, `ir-pass/SentinelIR.cpp`,
  the diagram in `docs/HOW_IT_WORKS.md` section 1.

## Module 3: Semantic analysis (AST checks)

- The AST plugin is a **semantic check on the syntax tree**. It runs after
  parsing and name resolution, when every `CallExpr` already knows which
  `FunctionDecl` it calls. A purely textual search couldn't do this reliably:
  for example, it would match `strcpy` inside a comment or string, and it would
  miss the macro-expanded `__builtin___strcpy_chk` form.
- Like a type checker, it walks the AST and emits **compile-time errors**
  through the compiler's diagnostic system (`DiagnosticsEngine`), with exact
  `file:line:col`.
- The `scanf("%s")` rule inspects a **constant (string literal) argument**
  during semantic analysis.
- Clang's own `-Wuninitialized`, which `sentinel-cc` turns into an error, is
  another semantic-analysis check. It is Clang's, not ours.
- Where: `ast-plugin/SentinelAST.cpp` (`BannedCallCallback::run`,
  `hasUnboundedPercentS`).

## Module 4: Intermediate code (LLVM IR as three-address code)

- LLVM IR is a typed **three-address code**. Each instruction has one
  operation and at most one result, e.g.
  `%arrayidx6 = getelementptr inbounds i8, ptr %call, i64 1` is
  `t = call + 1` in quadruple form.
- Our pass works only on IR, not on C source: `call @free(ptr %x)`, `load`,
  `store` and `getelementptr` are the IR forms of `free(x)`, reading,
  writing, and `x[i]` / `x->f`.
- `docs/HOW_IT_WORKS.md` section 3 shows real IR that clang generated for our
  test programs.

## Module 5: Basic blocks, flow graphs, data-flow analysis, security checking of VM code

- **Basic blocks and the flow graph:** LLVM groups IR into basic blocks
  (`BasicBlock`) linked by branches. We iterate over blocks and their
  **predecessors** (`predecessors(&BB)`), i.e. the edges of the CFG.
- **Data-flow analysis:** a textbook **forward, may** analysis:
  - IN[B] = ∪ OUT[P] over predecessors P;
  - OUT[B] = transfer(IN[B]), with gen = pointers freed in B and kill =
    values redefined in B;
  - iterated until a **fixpoint**, which is guaranteed by finite sets and
    monotone union.
  - Compare *reaching definitions* (also forward/may/union) and *available
    expressions* (forward/must/intersection). The worked example table for
    `uaf_one_branch.c` in `HOW_IT_WORKS.md` is the kind of IN/OUT table done
    in class.
- **Security checking of VM code:** LLVM IR is the code of the LLVM
  **virtual machine** (the "VM" in LLVM's original name). The pass checks this
  VM code for memory-safety violations (use-after-free, double-free, CWE-416 /
  CWE-415) before it is turned into machine code, and refuses to emit code if
  it finds any.
- Where: `ir-pass/SentinelIR.cpp` (`FreeChecker::run`, `computeIn`,
  `transfer`).

## Module 7: Static Single Assignment (SSA)

- Before the analysis we convert each function to **SSA form** with mem2reg
  (`PromoteMemToReg`). This uses the **dominator tree** to place **φ-nodes**.
- SSA is what makes the analysis precise enough. In SSA,
  `p = malloc(); free(p); p = malloc(); p[0] = 1;` becomes two different
  values `%call` and `%call1`, so the second use isn't confused with the freed
  first allocation (`tests/good/free_then_reassign.c`). `HOW_IT_WORKS.md`
  shows the real IR before and after mem2reg.
- φ-nodes are handled explicitly: a φ is "possibly freed" if the value
  arriving along some edge was freed on that edge.
- SSA also explains the *kill* rule: each SSA value has one definition, so
  re-executing that definition (in a loop) produces a fresh, not-freed value.
- Where: `promoteToSSA()` and the φ handling in `computeIn()` in
  `ir-pass/SentinelIR.cpp`.

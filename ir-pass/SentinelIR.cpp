//===- SentinelIR.cpp - SentinEL middle-end (LLVM IR) checker -----------===//
//
// A new-pass-manager plugin that finds, inside each function:
//
//   * double-free:     free(p) when p may already have been freed
//   * use-after-free:  load / store / GEP through p when p may be freed
//
// realloc(p, n) and reallocarray(p, n, size) count as freeing p: after the
// call only the returned pointer may be used. On the branch where the
// returned pointer is checked and found to be NULL, p is still valid.
//
// Load with:  clang -fpass-plugin=SentinelIR.dylib ...
// Warn-only:  set environment variable SENTINEL_WARN_ONLY=1
//
// Method: a forward "may-be-freed" data-flow analysis over basic blocks,
// on SSA form (mem2reg is run on a private copy of the module first).
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/StringSet.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/PromoteMemToReg.h"

#include <cstdlib>
#include <map>
#include <set>

using namespace llvm;

namespace {

// The data-flow fact: the set of SSA pointer values that MAY have been
// freed on some path reaching this point.
using FreedSet = std::set<const Value *>;

// Strip GEPs and casts so that p, &p[3] and (char *)p all map to p.
const Value *baseOf(const Value *Ptr) { return getUnderlyingObject(Ptr); }

// If I is a call to free(), return the pointer being freed.
const Value *freedPointer(const Instruction &I) {
  const auto *CB = dyn_cast<CallBase>(&I);
  if (!CB)
    return nullptr;
  const Function *Callee = CB->getCalledFunction();
  if (!Callee || Callee->getName() != "free" || CB->arg_size() != 1)
    return nullptr;
  return CB->getArgOperand(0);
}

// If I is a call to realloc(p, n) or reallocarray(p, n, size), return p.
// On success the old block is released, so p must not be used afterwards
// (CERT MEM30-C); only the returned pointer is valid.
const Value *reallocatedPointer(const Instruction &I) {
  const auto *CB = dyn_cast<CallBase>(&I);
  if (!CB)
    return nullptr;
  const Function *Callee = CB->getCalledFunction();
  if (!Callee)
    return nullptr;
  StringRef Name = Callee->getName();
  if ((Name == "realloc" && CB->arg_size() == 2) ||
      (Name == "reallocarray" && CB->arg_size() == 3))
    return CB->getArgOperand(0);
  return nullptr;
}

// A pointer that free / realloc / reallocarray invalidates, or nullptr.
const Value *invalidatedPointer(const Instruction &I) {
  if (const Value *P = freedPointer(I))
    return P;
  return reallocatedPointer(I);
}

// realloc() fails by returning NULL and leaving the old block untouched, so
//   q = realloc(p, n); if (q == NULL) { free(p); ... }
// is correct. If Pred ends in a branch on "realloc result == / != NULL",
// return the old pointer that is still valid along the edge Pred -> Succ.
const Value *stillValidOnEdge(const BasicBlock *Pred, const BasicBlock *Succ) {
  const auto *Br = dyn_cast<BranchInst>(Pred->getTerminator());
  if (!Br || !Br->isConditional() ||
      Br->getSuccessor(0) == Br->getSuccessor(1))
    return nullptr;
  const auto *Cmp = dyn_cast<ICmpInst>(Br->getCondition());
  if (!Cmp || !Cmp->isEquality())
    return nullptr;
  const Value *Other = nullptr;
  if (isa<ConstantPointerNull>(Cmp->getOperand(1)))
    Other = Cmp->getOperand(0);
  else if (isa<ConstantPointerNull>(Cmp->getOperand(0)))
    Other = Cmp->getOperand(1);
  const auto *Call = Other ? dyn_cast<Instruction>(Other) : nullptr;
  const Value *Old = Call ? reallocatedPointer(*Call) : nullptr;
  if (!Old)
    return nullptr;
  // "== NULL" is true on successor 0; "!= NULL" is true on successor 1.
  const BasicBlock *NullSucc = Br->getSuccessor(
      Cmp->getPredicate() == CmpInst::ICMP_EQ ? 0 : 1);
  return NullSucc == Succ ? baseOf(Old) : nullptr;
}

// If I accesses memory through a pointer, return that pointer.
const Value *accessedPointer(const Instruction &I) {
  if (const auto *L = dyn_cast<LoadInst>(&I))
    return L->getPointerOperand();
  if (const auto *S = dyn_cast<StoreInst>(&I))
    return S->getPointerOperand();
  if (const auto *G = dyn_cast<GetElementPtrInst>(&I))
    return G->getPointerOperand();
  return nullptr;
}

// "file.c:12:5" from the -g debug location, or "" if there is none.
std::string locString(const Instruction &I) {
  const DebugLoc &DL = I.getDebugLoc();
  if (!DL)
    return "";
  return (DL->getFilename() + ":" + Twine(DL->getLine()) + ":" +
          Twine(DL->getColumn()))
      .str();
}

std::string lineOf(const Instruction *I) {
  if (I && I->getDebugLoc())
    return "line " + std::to_string(I->getDebugLoc()->getLine());
  return "an earlier point";
}

class FreeChecker {
public:
  FreeChecker(Function &F, bool WarnOnly) : F(F), WarnOnly(WarnOnly) {}

  void run() {
    // Remember where each pointer is freed (only used in messages).
    for (Instruction &I : instructions(F))
      if (const Value *P = invalidatedPointer(I))
        FreeSite.emplace(baseOf(P), &I);

    // 1) Iterate to a fixpoint without reporting.
    //    OUT[B] = transfer(B, IN[B]);  IN[B] = union of OUT[pred].
    bool Changed = true;
    while (Changed) {
      Changed = false;
      for (BasicBlock &BB : F) {
        FreedSet Out = computeIn(BB);
        transfer(BB, Out, /*Report=*/false);
        if (Out != OutSets[&BB]) {
          OutSets[&BB] = std::move(Out);
          Changed = true;
        }
      }
    }

    // 2) One final walk with the stable IN sets, reporting problems.
    for (BasicBlock &BB : F) {
      FreedSet S = computeIn(BB);
      transfer(BB, S, /*Report=*/true);
    }
  }

private:
  // The freed set flowing along the edge Pred -> BB: OUT[Pred], except
  // that on the "realloc returned NULL" edge the old pointer is still live.
  FreedSet edgeOut(BasicBlock *Pred, const BasicBlock *BB) {
    FreedSet S = OutSets[Pred];
    if (const Value *Live = stillValidOnEdge(Pred, BB))
      S.erase(Live);
    return S;
  }

  // IN[B] = union of OUT[P] for every predecessor P (a "may" analysis:
  // freed on ANY incoming path counts as possibly freed).
  FreedSet computeIn(BasicBlock &BB) {
    FreedSet In;
    for (BasicBlock *Pred : predecessors(&BB)) {
      FreedSet PO = edgeOut(Pred, &BB);
      In.insert(PO.begin(), PO.end());
    }
    // A phi node merges pointers from different paths:
    //   p2 = phi [p0, %then], [p1, %else]
    // p2 is possibly freed if the value arriving on some edge was freed on
    // that edge.
    for (PHINode &Phi : BB.phis()) {
      In.erase(&Phi); // the phi gets a fresh value each time BB runs
      for (unsigned I = 0; I < Phi.getNumIncomingValues(); ++I) {
        FreedSet PO = edgeOut(Phi.getIncomingBlock(I), &BB);
        if (PO.count(baseOf(Phi.getIncomingValue(I))))
          In.insert(&Phi);
      }
    }
    return In;
  }

  // Walk the instructions of BB in order, updating the freed set S.
  void transfer(BasicBlock &BB, FreedSet &S, bool Report) {
    for (Instruction &I : BB) {
      if (const Value *P = invalidatedPointer(I)) {
        const Value *B = baseOf(P);
        if (Report && S.count(B))
          report(I, "double-free",
                 std::string(freedPointer(I)
                                 ? "pointer may already have been freed"
                                 : "realloc of a pointer that may already "
                                   "have been freed") +
                     " (at " +
                     lineOf(otherFreeSite(B, &I)) + ")",
                 "MEM30-C: set the pointer to NULL after free");
        S.insert(B);
      } else if (const Value *P = accessedPointer(I)) {
        const Value *B = baseOf(P);
        if (Report && S.count(B)) {
          const Instruction *Site = otherFreeSite(B, nullptr);
          bool ViaRealloc = Site && reallocatedPointer(*Site);
          report(I, "use-after-free",
                 "memory accessed through a pointer that may have been " +
                     std::string(ViaRealloc ? "reallocated" : "freed") +
                     " (at " + lineOf(Site) + ")",
                 ViaRealloc
                     ? "MEM30-C: after realloc, use only the pointer it returns"
                     : "MEM30-C: do not access freed memory");
        }
      }
      // In a loop an instruction can run again and produce a NEW value
      // (e.g. p = malloc(...) on the next iteration), which is not freed.
      S.erase(&I);
    }
  }

  // A free() / realloc() of B other than Current (for "previously freed at line N").
  const Instruction *otherFreeSite(const Value *B, const Instruction *Current) {
    auto Range = FreeSite.equal_range(B);
    for (auto It = Range.first; It != Range.second; ++It)
      if (It->second != Current)
        return It->second;
    return Current; // freed by the same free() on an earlier loop iteration
  }

  void report(const Instruction &I, StringRef Kind, const std::string &What,
              StringRef Rule) {
    std::string Loc = locString(I);
    // Report each kind at most once per source line, so p->x = 1 (a GEP
    // plus a store) gives one error, not two.
    std::string Key = Kind.str() + "@" +
                      (I.getDebugLoc() ? lineOf(&I)
                                       : std::to_string((uintptr_t)&I));
    if (!Reported.insert(Key).second)
      return;

    std::string Msg = (Loc.empty() ? "" : Loc + ": ") + "[SentinEL] " +
                      Kind.str() + " in function '" + F.getName().str() +
                      "': " + What + " (CERT " + Rule.str() + ")";
    LLVMContext &Ctx = F.getContext();
    if (WarnOnly)
      Ctx.diagnose(DiagnosticInfoGeneric(Msg, DS_Warning));
    else
      Ctx.emitError(Msg);
  }

  Function &F;
  bool WarnOnly;
  std::map<const BasicBlock *, FreedSet> OutSets;
  std::multimap<const Value *, const Instruction *> FreeSite;
  StringSet<> Reported;
};

// Turn stack variables (allocas) into SSA registers: the "mem2reg" step.
// After this, "p = malloc(); free(p); p = malloc();" gives two different
// SSA values for the two mallocs, which is what makes the analysis precise.
void promoteToSSA(Function &F) {
  std::vector<AllocaInst *> Allocas;
  for (Instruction &I : F.getEntryBlock())
    if (auto *AI = dyn_cast<AllocaInst>(&I))
      if (isAllocaPromotable(AI))
        Allocas.push_back(AI);
  if (Allocas.empty())
    return;
  DominatorTree DT(F);
  PromoteMemToReg(Allocas, DT);
}

struct SentinelPass : PassInfoMixin<SentinelPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
    bool WarnOnly = std::getenv("SENTINEL_WARN_ONLY") != nullptr;
    // Work on a private copy so the user's program (e.g. an -O0 debug build)
    // is compiled exactly as before; we only read the copy.
    std::unique_ptr<Module> Copy = CloneModule(M);
    for (Function &F : *Copy) {
      if (F.isDeclaration())
        continue;
      promoteToSSA(F);
      FreeChecker(F, WarnOnly).run();
    }
    return PreservedAnalyses::all();
  }
  static bool isRequired() { return true; } // run even on optnone functions
};

} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "SentinelIR", "1.0", [](PassBuilder &PB) {
            // PipelineStart runs at every -O level, including -O0, before
            // any optimisation has changed the code.
            PB.registerPipelineStartEPCallback(
                [](ModulePassManager &MPM, OptimizationLevel) {
                  MPM.addPass(SentinelPass());
                });
          }};
}

//===- SentinelAST.cpp - SentinEL front-end (AST) checker ---------------===//
//
// A Clang plugin that runs before code generation and reports an ERROR for
// every call to a banned, unbounded string/IO function:
//
//   gets, strcpy, strcat, sprintf, vsprintf, scanf/sscanf with bare "%s"
//
// Load with:  clang -fplugin=SentinelAST.dylib ...
// Warn-only:  -fplugin-arg-sentinel-warn-only
//
//===----------------------------------------------------------------------===//

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/Expr.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendPluginRegistry.h"

using namespace clang;
using namespace clang::ast_matchers;

namespace {

// One row per banned function: which CERT C rule it violates and what to
// use instead.
struct BannedFunc {
  const char *Name;
  const char *Cert;
  const char *Alternative;
};

const BannedFunc BannedTable[] = {
    {"gets", "MSC24-C, STR31-C", "fgets(buf, sizeof buf, stdin)"},
    {"strcpy", "STR31-C", "strlcpy(dst, src, sizeof dst) or strncpy + explicit NUL"},
    {"strcat", "STR31-C", "strlcat(dst, src, sizeof dst) or strncat"},
    {"sprintf", "STR31-C", "snprintf(buf, sizeof buf, ...)"},
    {"vsprintf", "STR31-C", "vsnprintf(buf, sizeof buf, ...)"},
};

// On macOS, <string.h>/<stdio.h> rewrite e.g. strcpy(...) into
// __builtin___strcpy_chk(...) when _FORTIFY_SOURCE is active (default at
// -O1 and above). Map those back to the user-visible name.
StringRef normalizeName(StringRef N) {
  if (N.consume_front("__builtin___") && N.consume_back("_chk"))
    return N;
  return N;
}

const BannedFunc *lookupBanned(StringRef Name) {
  for (const BannedFunc &B : BannedTable)
    if (Name == B.Name)
      return &B;
  return nullptr;
}

// True if a scanf-family format string has a "%s" with no field width,
// e.g. "%s" or "%d %s" (but not "%31s", "%*s" or "%%s").
bool hasUnboundedPercentS(StringRef Fmt) {
  for (size_t I = 0; I < Fmt.size(); ++I) {
    if (Fmt[I] != '%')
      continue;
    ++I;
    if (I >= Fmt.size())
      break;
    if (Fmt[I] == '%')          // "%%" is a literal percent sign
      continue;
    if (Fmt[I] == '*')          // "%*s" reads but does not store: safe
      continue;
    bool HasWidth = false;
    while (I < Fmt.size() && isdigit((unsigned char)Fmt[I])) {
      HasWidth = true;
      ++I;
    }
    if (I < Fmt.size() && Fmt[I] == 's' && !HasWidth)
      return true;
  }
  return false;
}

class BannedCallCallback : public MatchFinder::MatchCallback {
public:
  explicit BannedCallCallback(bool WarnOnly) : WarnOnly(WarnOnly) {}

  void run(const MatchFinder::MatchResult &Result) override {
    const auto *Call = Result.Nodes.getNodeAs<CallExpr>("call");
    const auto *Callee = Result.Nodes.getNodeAs<FunctionDecl>("callee");
    if (!Call || !Callee)
      return;

    DiagnosticsEngine &DE = Result.Context->getDiagnostics();
    auto Level = WarnOnly ? DiagnosticsEngine::Warning : DiagnosticsEngine::Error;
    StringRef Name = normalizeName(Callee->getName());
    SourceLocation Loc = Call->getBeginLoc();

    if (const BannedFunc *B = lookupBanned(Name)) {
      unsigned ID = DE.getCustomDiagID(
          Level, "[SentinEL] call to unsafe function '%0' (CERT %1); use %2 instead");
      DE.Report(Loc, ID) << B->Name << B->Cert << B->Alternative;
      return;
    }

    // scanf(fmt, ...) / sscanf(str, fmt, ...): inspect the format literal.
    unsigned FmtIdx = (Name == "scanf") ? 0 : 1;
    if (Call->getNumArgs() <= FmtIdx)
      return;
    const auto *Lit =
        dyn_cast<StringLiteral>(Call->getArg(FmtIdx)->IgnoreParenImpCasts());
    if (!Lit || !hasUnboundedPercentS(Lit->getString()))
      return;
    unsigned ID = DE.getCustomDiagID(
        Level, "[SentinEL] '%0' with unbounded \"%%s\" can overflow the "
               "buffer (CERT STR31-C); give a width like \"%%31s\" or use fgets");
    DE.Report(Loc, ID) << Name;
  }

private:
  bool WarnOnly;
};

// The consumer owns the MatchFinder. (Clang destroys the PluginASTAction
// object right after CreateASTConsumer returns, so nothing the consumer
// needs may live in the action.)
class SentinelConsumer : public ASTConsumer {
public:
  explicit SentinelConsumer(bool WarnOnly) : Callback(WarnOnly) {
    // callExpr(...): any function call whose callee is a declared function
    // with one of these names, not coming from a system header.
    Finder.addMatcher(
        callExpr(callee(functionDecl(hasAnyName(
                            "gets", "strcpy", "strcat", "sprintf", "vsprintf",
                            "__builtin___strcpy_chk", "__builtin___strcat_chk",
                            "__builtin___sprintf_chk", "__builtin___vsprintf_chk",
                            "scanf", "sscanf"))
                            .bind("callee")),
                 unless(isExpansionInSystemHeader()))
            .bind("call"),
        &Callback);
  }

  // Called once the whole file has been parsed into an AST.
  void HandleTranslationUnit(ASTContext &Ctx) override { Finder.matchAST(Ctx); }

private:
  BannedCallCallback Callback;
  MatchFinder Finder;
};

class SentinelASTAction : public PluginASTAction {
public:
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &,
                                                 StringRef) override {
    return std::make_unique<SentinelConsumer>(WarnOnly);
  }

  bool ParseArgs(const CompilerInstance &,
                 const std::vector<std::string> &Args) override {
    for (const std::string &A : Args)
      if (A == "warn-only")
        WarnOnly = true;
    return true;
  }

  // Run automatically with -fplugin, before normal code generation.
  ActionType getActionType() override { return AddBeforeMainAction; }

private:
  bool WarnOnly = false;
};

} // namespace

static FrontendPluginRegistry::Add<SentinelASTAction>
    X("sentinel", "SentinEL: reject banned unsafe C library calls");

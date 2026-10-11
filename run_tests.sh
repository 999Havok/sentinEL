#!/usr/bin/env bash
# Compile every program in tests/bad and tests/good with sentinel-cc and
# print a table: file, expected, actual, pass/fail.
#
#   tests/bad/*   must be REJECTED, and by the right check:
#                   banned_*       -> AST   (SentinelAST plugin)
#                   double_free_*  -> IR    (SentinelIR pass)
#                   uaf_*          -> IR    (SentinelIR pass)
#                   uninit_*       -> UNINIT (Clang's -Wuninitialized)
#   tests/good/*  must be ACCEPTED with no SentinEL / uninitialized diagnostic.
#
# Output is also saved to results/test_results.txt.

set -uo pipefail
cd "$(dirname "$0")"

CC=./sentinel-cc
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p results
OUT=results/test_results.txt

expected_check() {
  case "$(basename "$1")" in
    banned_*) echo AST ;;
    double_free_* | uaf_*) echo IR ;;
    uninit_*) echo UNINIT ;;
    *) echo "?" ;;
  esac
}

# Which of our checks produced diagnostics in this compiler output?
checks_fired() {
  local log="$1" fired=""
  grep -q "\[SentinEL\] call to unsafe\|\[SentinEL\] '.*' with unbounded" "$log" && fired+="AST,"
  grep -q "\[SentinEL\] \(double-free\|use-after-free\)" "$log" && fired+="IR,"
  grep -q "uninitialized" "$log" && fired+="UNINIT,"
  echo "${fired%,}"
}

pass=0
fail=0
{
  printf "%-32s %-16s %-24s %s\n" "FILE" "EXPECTED" "ACTUAL" "RESULT"
  printf "%-32s %-16s %-24s %s\n" "----" "--------" "------" "------"
  for f in tests/bad/*.c tests/good/*.c; do
    log="$TMP/log"
    if $CC -c "$f" -o "$TMP/out.o" >"$log" 2>&1; then status=ACCEPTED; else status=REJECTED; fi
    fired="$(checks_fired "$log")"

    if [[ "$f" == tests/bad/* ]]; then
      want="$(expected_check "$f")"
      expected="REJECTED($want)"
      [[ $status == REJECTED && ",$fired," == *",$want,"* ]] && ok=1 || ok=0
    else
      expected="ACCEPTED"
      [[ $status == ACCEPTED && -z "$fired" ]] && ok=1 || ok=0
    fi
    actual="$status${fired:+($fired)}"

    if [[ $ok -eq 1 ]]; then result=PASS; pass=$((pass + 1)); else result=FAIL; fail=$((fail + 1)); fi
    printf "%-32s %-16s %-24s %s\n" "${f#tests/}" "$expected" "$actual" "$result"
  done
  echo
  echo "Total: $((pass + fail))   Passed: $pass   Failed: $fail"
} >"$OUT"   # not "| tee": a pipe would run the loop in a subshell and lose $fail

cat "$OUT"
[[ $fail -eq 0 ]]

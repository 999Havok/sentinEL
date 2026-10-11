#!/usr/bin/env python3
"""Compile-time benchmark: plain clang vs sentinel-cc.

Compiles every program in tests/good (they compile under all configs) with
  1. clang          (Homebrew clang, same -isysroot as sentinel-cc)
  2. clang -g       (isolates the cost of the -g that sentinel-cc adds)
  3. clang+plugins  (exactly the flags sentinel-cc adds, but calling clang
                     directly: isolates the cost of the bash wrapper)
  4. sentinel-cc    (both plugins + -g + -Werror=uninitialized, via the script)
at -O0 and -O2, RUNS times each, and reports the average wall-clock time to
compile the whole set and the overhead relative to plain clang.

Usage:  ./benchmark.py [RUNS]        (default 10)
Writes: results/benchmark.txt and results/benchmark.csv
"""
import datetime
import glob
import os
import platform
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.abspath(__file__))
CLANG = "/opt/homebrew/opt/llvm/bin/clang"
SENTINEL = os.path.join(ROOT, "sentinel-cc")
RUNS = int(sys.argv[1]) if len(sys.argv) > 1 else 10

SDK = subprocess.check_output(["xcrun", "--show-sdk-path"], text=True).strip()
FILES = sorted(glob.glob(os.path.join(ROOT, "tests", "good", "*.c")))

CONFIGS = {
    "clang": [CLANG, "-isysroot", SDK],
    "clang -g": [CLANG, "-isysroot", SDK, "-g"],
    "clang+plugins": [CLANG, "-isysroot", SDK, "-g",
                      "-fplugin=" + os.path.join(ROOT, "build/lib/SentinelAST.dylib"),
                      "-fpass-plugin=" + os.path.join(ROOT, "build/lib/SentinelIR.dylib"),
                      "-Wuninitialized", "-Werror=uninitialized"],
    "sentinel-cc": [SENTINEL],
}
OPT_LEVELS = ["-O0", "-O2"]


def compile_all(cmd, opt, outdir):
    """Compile every file once; return elapsed seconds."""
    start = time.perf_counter()
    for i, f in enumerate(FILES):
        out = os.path.join(outdir, f"{i}.o")
        subprocess.run(cmd + [opt, "-c", f, "-o", out], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return time.perf_counter() - start


def main():
    times = {(c, o): [] for c in CONFIGS for o in OPT_LEVELS}
    with tempfile.TemporaryDirectory() as tmp:
        # One warm-up round (fills OS file caches), not measured.
        for cmd in CONFIGS.values():
            for opt in OPT_LEVELS:
                compile_all(cmd, opt, tmp)
        # Interleave configurations so slow drift affects all of them equally.
        for _ in range(RUNS):
            for name, cmd in CONFIGS.items():
                for opt in OPT_LEVELS:
                    times[(name, opt)].append(compile_all(cmd, opt, tmp))

    clang_ver = subprocess.check_output([CLANG, "--version"], text=True).splitlines()[0]
    lines = [
        "SentinEL compile-time benchmark",
        f"date:      {datetime.datetime.now():%Y-%m-%d %H:%M}",
        f"machine:   {platform.machine()} macOS {platform.mac_ver()[0]}",
        f"compiler:  {clang_ver}",
        f"programs:  {len(FILES)} files from tests/good, compiled with -c",
        f"runs:      {RUNS} per configuration (+1 warm-up, not counted)",
        "",
        f"{'opt':<5}{'config':<15}{'avg ms/set':>12}{'stdev ms':>10}"
        f"{'avg ms/file':>13}{'vs clang':>10}{'vs clang -g':>13}",
    ]
    csv = ["opt,config,avg_ms_per_set,stdev_ms,avg_ms_per_file,overhead_vs_clang_pct,overhead_vs_clang_g_pct"]
    for opt in OPT_LEVELS:
        base = statistics.mean(times[("clang", opt)])
        base_g = statistics.mean(times[("clang -g", opt)])
        for name in CONFIGS:
            t = times[(name, opt)]
            avg, sd = statistics.mean(t), statistics.stdev(t) if len(t) > 1 else 0.0
            ov = (avg / base - 1) * 100
            ov_g = (avg / base_g - 1) * 100
            lines.append(f"{opt:<5}{name:<15}{avg*1000:>12.1f}{sd*1000:>10.1f}"
                         f"{avg*1000/len(FILES):>13.1f}{ov:>9.1f}%{ov_g:>12.1f}%")
            csv.append(f"{opt},{name},{avg*1000:.2f},{sd*1000:.2f},"
                       f"{avg*1000/len(FILES):.2f},{ov:.2f},{ov_g:.2f}")
    lines += ["",
              "'vs clang'    = total overhead of using sentinel-cc instead of clang.",
              "'vs clang -g' = overhead beyond the -g that sentinel-cc adds.",
              "clang+plugins vs clang -g = cost of the SentinEL checks themselves;",
              "sentinel-cc vs clang+plugins = cost of the bash wrapper script."]

    os.makedirs(os.path.join(ROOT, "results"), exist_ok=True)
    with open(os.path.join(ROOT, "results", "benchmark.txt"), "w") as fh:
        fh.write("\n".join(lines) + "\n")
    with open(os.path.join(ROOT, "results", "benchmark.csv"), "w") as fh:
        fh.write("\n".join(csv) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()

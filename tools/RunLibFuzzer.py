#!/usr/bin/env python3
"""libFuzzer coverage-guided automated fuzzing runner for Razobill.

Builds tests/fuzz/FuzzDecomposeReconstruct.cpp under Clang with AddressSanitizer,
UndefinedBehaviorSanitizer, and libFuzzer instrumentation (-fsanitize=fuzzer),
populates the initial seed corpus from tests/fuzz/seeds/seed_logs.txt, and
executes continuous coverage-guided mutation with tests/fuzz/razobill.dict.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


def prepare_seeds(seed_txt: Path, corpus_dir: Path) -> int:
    corpus_dir.mkdir(parents=True, exist_ok=True)
    if not seed_txt.exists():
        return 0
    lines = seed_txt.read_text(encoding="utf-8").splitlines()
    count = 0
    for i, line in enumerate(lines):
        line = line.strip()
        if not line:
            continue
        seed_file = corpus_dir / f"seed_{i:04d}.bin"
        seed_file.write_bytes(line.encode("utf-8"))
        count += 1
    return count


def main() -> int:
    parser = argparse.ArgumentParser(description="Run libFuzzer on Razobill.")
    parser.add_argument(
        "--duration",
        type=int,
        default=60,
        help="Max fuzzing duration in seconds (default: 60)",
    )
    parser.add_argument(
        "--workers",
        type=int,
        default=0,
        help="Number of parallel fuzzer workers (default: 0 for in-process)",
    )
    parser.add_argument(
        "--runs",
        type=int,
        default=-1,
        help="Number of runs (-1 for unlimited within duration)",
    )
    parser.add_argument(
        "--build-dir",
        type=str,
        default="build-libfuzzer",
        help="Build directory for libFuzzer target",
    )
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    seed_txt = repo_root / "tests" / "fuzz" / "seeds" / "seed_logs.txt"
    corpus_dir = repo_root / "tests" / "fuzz" / "corpus"
    dict_file = repo_root / "tests" / "fuzz" / "razobill.dict"

    seed_count = prepare_seeds(seed_txt, corpus_dir)
    print(f"[RunLibFuzzer] Prepared {seed_count} seed inputs in {corpus_dir}")

    # Check for clang++
    clang_cxx = shutil.which("clang++")
    if not clang_cxx:
        print("[RunLibFuzzer] Warning: clang++ not found on PATH. Checking clang...")
        clang_cxx = shutil.which("clang")
    if not clang_cxx:
        print(
            "[RunLibFuzzer] Error: LLVM Clang is required for libFuzzer coverage instrumentation.",
            file=sys.stderr,
        )
        return 1

    build_path = repo_root / args.build_dir
    os.environ["CC"] = "clang"
    os.environ["CXX"] = "clang++"

    print(f"[RunLibFuzzer] Configuring CMake in {build_path} with Clang...")
    cmd_cmake = [
        "cmake",
        "-B",
        str(build_path),
        "-G",
        "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_CXX_COMPILER=clang++",
        "-DENABLE_FUZZING=ON",
    ]
    ret = subprocess.run(cmd_cmake, cwd=str(repo_root)).returncode
    if ret != 0:
        print("[RunLibFuzzer] CMake configuration failed.", file=sys.stderr)
        return ret

    print("[RunLibFuzzer] Compiling FuzzDecomposeReconstruct...")
    cmd_build = ["ninja", "-C", str(build_path), "FuzzDecomposeReconstruct"]
    ret = subprocess.run(cmd_build, cwd=str(repo_root)).returncode
    if ret != 0:
        print("[RunLibFuzzer] Compilation failed.", file=sys.stderr)
        return ret

    fuzzer_bin = build_path / "FuzzDecomposeReconstruct"
    if not fuzzer_bin.exists():
        fuzzer_bin = build_path / "FuzzDecomposeReconstruct.exe"

    if not fuzzer_bin.exists():
        print(f"[RunLibFuzzer] Executable not found at {fuzzer_bin}", file=sys.stderr)
        return 1

    fuzz_args = [
        str(fuzzer_bin),
        str(corpus_dir),
        f"-dict={dict_file}",
        f"-max_total_time={args.duration}",
    ]
    if args.workers > 1:
        fuzz_args.append(f"-workers={args.workers}")
        fuzz_args.append(f"-jobs={args.workers}")
    if args.runs > 0:
        fuzz_args.append(f"-runs={args.runs}")

    print(f"[RunLibFuzzer] Launching libFuzzer for {args.duration}s: {' '.join(fuzz_args)}")
    return subprocess.run(fuzz_args, cwd=str(repo_root)).returncode


if __name__ == "__main__":
    sys.exit(main())

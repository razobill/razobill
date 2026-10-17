#!/usr/bin/env python3
# =============================================================================
# end-to-end benchmark and profiling orchestration suite
#
# evaluates cold vs warm decomposition, pinned core latency, multi-threaded
# scaling speedup, and native BenchCore execution across real Loghub datasets
# ~ orchestrates all benchmarks and writes verified performance JSON reports
# =============================================================================

import os
import sys
import json
import time
import hashlib
import platform
import subprocess
from statistics import median
from pathlib import Path

try:
    import psutil
except ImportError:
    psutil = None

# Ensure pyrazobill is on path
REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "lib" / "python"))

import pyrazobill

assert getattr(pyrazobill, "_NATIVE", False), "FATAL: pyrazobill native C++ extension must be loaded for benchmarks!"

DATASETS = [
    {
        "name": "SSH",
        "path": "logs/SSH/SSH.log",
        "description": "OpenSSH auth & session log corpus (Loghub)",
        "sample_lines": 50000,
    },
    {
        "name": "Linux",
        "path": "logs/Linux/Linux.log",
        "description": "Linux system / syslog kernel events (Loghub)",
        "sample_lines": 25000,
    },
    {
        "name": "Zookeeper",
        "path": "logs/Zookeeper/Zookeeper.log",
        "description": "Apache Zookeeper distributed coordination log (Loghub)",
        "sample_lines": 50000,
    },
]


def get_git_commit() -> str:
    """read current git revision sha via git rev-parse."""
    try:
        res = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=str(REPO_ROOT),
            capture_output=True,
            text=True,
            check=True,
        )
        return res.stdout.strip()
    except Exception:
        return "unknown"


def get_compiler_info() -> str:
    """read host C++ compiler version string from gcc."""
    try:
        res = subprocess.run(
            ["gcc", "--version"],
            capture_output=True,
            text=True,
            check=True,
        )
        return res.stdout.splitlines()[0]
    except Exception:
        return "GCC (MSYS2 UCRT64)"


def hash_file(path: Path) -> tuple[str, int, int]:
    """compute SHA-256 hash, byte count, and newline count of target file.

    in: path points to readable file on disk
    out: tuple of (sha256_hex, total_bytes, line_count)
    """
    sha = hashlib.sha256()
    line_count = 0
    byte_count = 0
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            sha.update(chunk)
            byte_count += len(chunk)
            line_count += chunk.count(b"\n")
    return sha.hexdigest(), byte_count, line_count


def pin_to_core(core_id: int = 0):
    """pin current process execution to specified logical core ID."""
    if psutil:
        p = psutil.Process()
        p.cpu_affinity([core_id])


def unpin_cores():
    """reset process CPU affinity to all available logical cores."""
    if psutil:
        p = psutil.Process()
        p.cpu_affinity(list(range(psutil.cpu_count(logical=True))))


def profile_component_breakdown(lines: list[str], runs: int = 5) -> dict:
    """profile individual stages of the Razobill pipeline on pinned core.

    in: lines non-empty list of string log events; runs >= 1
    out: dict of per-stage timing, throughput, and jitter spread metrics
    """
    pin_to_core(0)
    total_bytes = sum(len(line) for line in lines)
    n = len(lines)

    stages = [
        ("parse_exact", "exact"),
        ("parse_semantic", "semantic"),
    ]

    results = {}

    for name, mode in stages:
        durations = []
        for r in range(runs):
            t0 = time.perf_counter()
            for line in lines:
                pyrazobill.RzSplit(line, mode=mode)
            durations.append(time.perf_counter() - t0)

        med_sec = median(durations)
        results[name] = {
            "median_sec": med_sec,
            "ns_per_msg": (med_sec / n) * 1e9,
            "mb_per_sec": (total_bytes / med_sec) / 1e6,
            "msgs_per_sec": n / med_sec,
            "spread_pct": ((max(durations) - min(durations)) / med_sec) * 100 if med_sec > 0 else 0,
        }

    # Breakdown: parse + formKey vs reconstruct
    reconstruct_durations = []
    records = [pyrazobill.RzSplit(line) for line in lines]
    for r in range(runs):
        t0 = time.perf_counter()
        for rec in records:
            _ = pyrazobill.RzJoin(rec.template, rec.ints, rec.strs)
        reconstruct_durations.append(time.perf_counter() - t0)

    med_rec = median(reconstruct_durations)
    results["reconstruct_only"] = {
        "median_sec": med_rec,
        "ns_per_msg": (med_rec / n) * 1e9,
        "mb_per_sec": (total_bytes / med_rec) / 1e6,
        "msgs_per_sec": n / med_rec,
        "spread_pct": ((max(reconstruct_durations) - min(reconstruct_durations)) / med_rec) * 100 if med_rec > 0 else 0,
    }

    # Full round-trip
    rt_durations = []
    for r in range(runs):
        t0 = time.perf_counter()
        for line in lines:
            rec = pyrazobill.RzSplit(line)
            _ = pyrazobill.RzJoin(rec.template, rec.ints, rec.strs)
        rt_durations.append(time.perf_counter() - t0)

    med_rt = median(rt_durations)
    results["round_trip_parse_reconstruct"] = {
        "median_sec": med_rt,
        "ns_per_msg": (med_rt / n) * 1e9,
        "mb_per_sec": (total_bytes / med_rt) / 1e6,
        "msgs_per_sec": n / med_rt,
        "spread_pct": ((max(rt_durations) - min(rt_durations)) / med_rt) * 100 if med_rt > 0 else 0,
    }

    unpin_cores()
    return results


def benchmark_multicore_scaling(lines: list[str], thread_counts: list[int], runs: int = 5) -> dict:
    """measure multi-core scaling efficiency using thread pools across chunks.

    in: lines log corpus lines; thread_counts list of thread pool sizes
    out: dict mapping thread count keys to throughput and speedup stats
    """
    unpin_cores()
    scaling_results = {}
    baseline_throughput = 0.0

    for t in thread_counts:
        run_rates = []
        run_mb = []
        chunk_size = (len(lines) + t - 1) // t
        chunks = [lines[i * chunk_size : min((i + 1) * chunk_size, len(lines))] for i in range(t)]
        chunks = [c for c in chunks if c]
        total_bytes = sum(len(line) for line in lines)
        n = len(lines)

        from concurrent.futures import ThreadPoolExecutor

        for r in range(runs):
            t0 = time.perf_counter()
            with ThreadPoolExecutor(max_workers=t) as pool:
                list(pool.map(pyrazobill.RzSplitMany, chunks))
            elapsed = time.perf_counter() - t0
            run_rates.append(n / elapsed if elapsed > 0 else 0.0)
            run_mb.append((total_bytes / elapsed) / 1e6 if elapsed > 0 else 0.0)

        med_msgs_per_sec = median(run_rates)
        med_mb_per_sec = median(run_mb)
        if t == 1:
            baseline_throughput = med_msgs_per_sec

        speedup = (med_msgs_per_sec / baseline_throughput) if baseline_throughput > 0 else 1.0
        efficiency = (speedup / t) * 100.0

        scaling_results[f"{t}_threads"] = {
            "threads": t,
            "median_msgs_per_sec": med_msgs_per_sec,
            "median_mb_per_sec": med_mb_per_sec,
            "min_msgs_per_sec": min(run_rates),
            "max_msgs_per_sec": max(run_rates),
            "spread_pct": ((max(run_rates) - min(run_rates)) / med_msgs_per_sec) * 100.0
            if med_msgs_per_sec > 0
            else 0.0,
            "speedup": speedup,
            "efficiency_pct": efficiency,
        }

    return scaling_results


def run_benchcore_executable(corpus_path: str, max_lines: int) -> str:
    """execute native compiled BenchCore binary and capture stdout output.

    in: corpus_path path to input log file; max_lines line cap
    out: stdout string from BenchCore execution
    err: raises FileNotFoundError if binary missing, RuntimeError on failure
    """
    exe_name = "BenchCore.exe" if os.name == "nt" else "BenchCore"
    bench_exe = REPO_ROOT / "build" / exe_name
    if not bench_exe.exists():
        raise FileNotFoundError(f"{bench_exe} not built")

    try:
        env = os.environ.copy()
        if os.name == "nt":
            env["PATH"] = f"C:\\Msys64\\mingw64\\bin;{env.get('PATH', '')}"
        res = subprocess.run(
            [str(bench_exe), corpus_path, str(max_lines)],
            capture_output=True,
            text=True,
            env=env,
            check=True,
        )
        return res.stdout
    except subprocess.CalledProcessError as e:
        raise RuntimeError(
            f"BenchCore failed on {corpus_path} (exit {e.returncode}):\nStdout: {e.stdout}\nStderr: {e.stderr}"
        ) from e


def parse_benchcore_output(stdout: str) -> dict:
    """parse tabular throughput and latency metrics from BenchCore stdout.

    in: stdout string output produced by BenchCore
    out: dict structured with single-core ops and parallel scaling metrics
    """
    import re

    metrics = {
        "single_core_ops": {},
        "parallel_scaling": {},
    }
    for line in stdout.splitlines():
        line = line.strip()
        m_op = re.match(r"^(.+?)\s+([\d\.]+)\s+ns/msg\s+([\d\.]+)\s+MB/s\s+([\d\.]+)\s+M msg/s", line)
        if m_op:
            op_name = m_op.group(1).strip()
            metrics["single_core_ops"][op_name] = {
                "ns_per_msg": float(m_op.group(2)),
                "mb_per_sec": float(m_op.group(3)),
                "mega_msgs_per_sec": float(m_op.group(4)),
            }
            continue
        m_sc = re.match(
            r"^(\d+)\s+Cores\s*\(\s*(\d+)\s+Threads\)\s+([\d\.]+)\s+MB/s\s+([\d\.]+)\s+M msg/s\s+\(Speedup\s+([\d\.]+)x(?:,\s*sink\s+\d+)?\)",
            line,
        )
        if m_sc:
            cores = int(m_sc.group(1))
            threads = int(m_sc.group(2))
            metrics["parallel_scaling"][f"{threads}_threads"] = {
                "cores": cores,
                "threads": threads,
                "mb_per_sec": float(m_sc.group(3)),
                "mega_msgs_per_sec": float(m_sc.group(4)),
                "speedup": float(m_sc.group(5)),
            }
    return metrics


def main():
    """execute benchmark suite across datasets and emit benchmarks.json."""
    print("=" * 80)
    print("RAZOBILL PHASE 3 PERFORMANCE AUDIT & PROFILE SUITE")
    print("=" * 80)

    git_commit = get_git_commit()
    compiler_info = get_compiler_info()
    cpu_model = platform.processor() or "AMD Ryzen / Intel x86_64"
    if not psutil:
        print("[WARNING] psutil is not available; CPU pinning disabled and hardware memory will not be reported.")
    logical_cores = psutil.cpu_count(logical=True) if psutil else (os.cpu_count() or 1)
    physical_cores = psutil.cpu_count(logical=False) if psutil else None
    total_ram_gb = round(psutil.virtual_memory().total / (1024**3), 2) if psutil else None

    system_metadata = {
        "git_commit": git_commit,
        "os": f"{platform.system()} {platform.release()} ({platform.machine()})",
        "cpu_model": cpu_model,
        "physical_cores": physical_cores,
        "logical_cores": logical_cores,
        "ram_gb": total_ram_gb,
        "compiler": compiler_info,
        "python_version": sys.version.split()[0],
        "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }

    print(f"Commit    : {git_commit}")
    print(f"Platform  : {system_metadata['os']}")
    print(f"Hardware  : {cpu_model} ({physical_cores}P / {logical_cores}T, {total_ram_gb} GB RAM)")
    print(f"Compiler  : {compiler_info}")
    print("-" * 80)

    bench_data = {
        "metadata": system_metadata,
        "datasets": {},
    }

    for d in DATASETS:
        file_path = REPO_ROOT / d["path"]
        if not file_path.exists():
            print(f"[SKIP] Dataset {d['name']} not found at {file_path}")
            continue

        sha256, byte_count, line_count = hash_file(file_path)
        print(f"\nProcessing Dataset: {d['name']} ({byte_count / 1e6:.2f} MB, {line_count} lines)")
        print(f"SHA-256: {sha256}")

        with open(file_path, "r", encoding="utf-8", errors="replace") as f:
            lines = [line.rstrip("\r\n") for line in f if line.strip()][: d["sample_lines"]]

        print(f"Benchmark slice: {len(lines)} lines loaded")

        # 1. Cold vs Warm Parsing on Core 0
        pin_to_core(0)
        t_cold_start = time.perf_counter()
        for line in lines:
            _ = pyrazobill.RzSplit(line)
        cold_sec = time.perf_counter() - t_cold_start

        warm_durations = []
        for _ in range(5):
            t_w = time.perf_counter()
            for line in lines:
                _ = pyrazobill.RzSplit(line)
            warm_durations.append(time.perf_counter() - t_w)
        warm_sec = median(warm_durations)
        unpin_cores()

        cold_warm_metrics = {
            "cold_sec": cold_sec,
            "cold_msgs_per_sec": len(lines) / cold_sec,
            "warm_sec": warm_sec,
            "warm_msgs_per_sec": len(lines) / warm_sec,
            "warmup_speedup": cold_sec / warm_sec if warm_sec > 0 else 1.0,
        }

        # 2. Component Breakdown & Bottleneck Profiling
        print("  -> Running component breakdown profiling (Core 0 pinned)...")
        breakdown = profile_component_breakdown(lines, runs=5)

        # 3. Multi-Core Scaling
        thread_counts = sorted(list(set(min(t, logical_cores) for t in [1, 2, 4, 6, 12])))
        print(f"  -> Running multi-core scaling across threads: {thread_counts}...")
        multicore = benchmark_multicore_scaling(lines, thread_counts, runs=5)

        # 4. Native C++ Executable Output
        print("  -> Executing native BenchCore.exe microbenchmark...")
        native_output = run_benchcore_executable(str(file_path), d["sample_lines"])
        native_metrics = parse_benchcore_output(native_output)

        dataset_result = {
            "dataset_info": {
                "name": d["name"],
                "path": d["path"],
                "description": d["description"],
                "total_file_bytes": byte_count,
                "total_file_lines": line_count,
                "sha256": sha256,
                "benchmarked_lines": len(lines),
            },
            "cold_vs_warm": cold_warm_metrics,
            "component_breakdown": breakdown,
            "multicore_scaling": multicore,
            "native_benchcore_metrics": native_metrics,
            "native_benchcore_output": native_output,
        }

        bench_data["datasets"][d["name"]] = dataset_result

        # Display concise summary
        print(f"\n  Summary for {d['name']}:")
        print(f"    Cold parse throughput  : {cold_warm_metrics['cold_msgs_per_sec'] / 1e6:.2f} M msg/s")
        print(f"    Warm parse throughput  : {cold_warm_metrics['warm_msgs_per_sec'] / 1e6:.2f} M msg/s")
        print(f"    Reconstruct throughput : {breakdown['reconstruct_only']['msgs_per_sec'] / 1e6:.2f} M msg/s")
        print(
            f"    Full Round-Trip        : {breakdown['round_trip_parse_reconstruct']['msgs_per_sec'] / 1e6:.2f} M msg/s"
        )
        for k, v in multicore.items():
            print(
                f"    Multi-core {v['threads']:2d} threads  : {v['median_msgs_per_sec'] / 1e6:.2f} M msg/s ({v['median_mb_per_sec']:.1f} MB/s) | Speedup {v['speedup']:.2f}x ({v['efficiency_pct']:.1f}%)"
            )

    # Save JSON report
    out_dir = REPO_ROOT / "bench" / "results"
    out_dir.mkdir(parents=True, exist_ok=True)
    out_file = out_dir / "benchmarks.json"

    with open(out_file, "w", encoding="utf-8") as f:
        json.dump(bench_data, f, indent=2)

    print("\n" + "=" * 80)
    print(f"BENCHMARK COMPLETE. Results written to: {out_file}")
    print("=" * 80)


if __name__ == "__main__":
    main()

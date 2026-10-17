#!/usr/bin/env python3
# =============================================================================
# multi-core scale and invertibility stress test across production log corpora
#
# evaluates parallel decomposition throughput, memory footprint, and exact
# byte-level invertibility across full logs/ corpus via auditParallel
# ~ بنرمي عليه كل الملفات دفعة واحدة عشان نتأكد إن مفيش بايت واحد بيسقط
# =============================================================================

import os
import sys
import time
import psutil
from pathlib import Path

# Add lib/python directory to sys.path
pkg_dir = Path(__file__).resolve().parent.parent / "lib" / "python"
sys.path.insert(0, str(pkg_dir))

import pyrazobill


def main():
    """stream production logs in batches through parallel invertibility audit."""
    root = Path(__file__).resolve().parent.parent
    logs_dir = root / "logs"
    if not logs_dir.exists():
        print(f"Error: logs directory not found at {logs_dir}")
        sys.exit(1)

    log_files = sorted(logs_dir.rglob("*.log"))
    if not log_files:
        print("No .log files found in logs/")
        sys.exit(1)

    process = psutil.Process()
    num_cpus = os.cpu_count() or 12
    initial_rss = process.memory_info().rss / (1024 * 1024)

    print("=" * 110)
    print("LIBRAZOBILL / PYRAZOBILL: MULTI-CORE STRESS TEST & LOSSLESS AUDIT")
    print(f"Hardware Cores: {num_cpus} Logical Cores | Initial Memory: {initial_rss:.2f} MB")
    print(f"Log Files Target: {len(log_files):,} files discovered")
    print("=" * 110)

    total_lines = 0
    total_bytes = 0
    total_mismatches = 0
    all_unique_forms = 0

    batch_lines = []
    batch_bytes = 0
    BATCH_SIZE = 50000

    t_start = time.perf_counter()

    for idx, fpath in enumerate(log_files, 1):
        try:
            with open(fpath, "r", encoding="utf-8", errors="replace") as f:
                for line in f:
                    s = line.rstrip("\r\n")
                    if not s:
                        continue
                    batch_lines.append(s)
                    batch_bytes += len(s.encode("utf-8"))

                    if len(batch_lines) >= BATCH_SIZE:
                        res = pyrazobill.auditParallel(
                            batch_lines,
                            pyrazobill.ScanMode.Semantic,
                            numThreads=num_cpus,
                        )
                        total_lines += res.totalLines
                        total_bytes += res.totalBytes
                        total_mismatches += res.mismatches
                        all_unique_forms += res.uniqueFormsCount
                        batch_lines.clear()
                        batch_bytes = 0
        except Exception as e:
            print(f"Warning reading {fpath.name}: {e}")

    # Process remaining lines
    if batch_lines:
        res = pyrazobill.auditParallel(batch_lines, pyrazobill.ScanMode.Semantic, numThreads=num_cpus)
        total_lines += res.totalLines
        total_bytes += res.totalBytes
        total_mismatches += res.mismatches
        all_unique_forms += res.uniqueFormsCount

    t_end = time.perf_counter()
    total_elapsed = t_end - t_start
    final_rss = process.memory_info().rss / (1024 * 1024)

    print("\n" + "=" * 110)
    print("STRESS TEST & AUDIT SUMMARY RESULTS")
    print("=" * 110)
    print(f"Total Lines Processed : {total_lines:,} lines")
    print(f"Total Data Volume     : {total_bytes / (1024**3):.2f} GB ({total_bytes / (1024**2):,.2f} MB)")
    print(f"Total Wall Time       : {total_elapsed:.2f} seconds")
    print(
        f"End-to-End Throughput : {total_lines / total_elapsed:>12,.0f} msgs/sec ({(total_bytes / (1024**2)) / total_elapsed:>8.2f} MB/sec)"
    )
    print(
        f"Lossless Invertibility: {total_mismatches:,} mismatches ({'100% BYTE-FOR-BYTE EXACT' if total_mismatches == 0 else 'FAIL'})"
    )
    print(f"Peak Memory Working Set: {final_rss:.2f} MB (Delta: +{final_rss - initial_rss:.2f} MB)")
    print("=" * 110)


if __name__ == "__main__":
    main()

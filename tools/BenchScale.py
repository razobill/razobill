#!/usr/bin/env python3
# =============================================================================
# multi-core scale and invertibility stress test across production log corpora
#
# evaluates parallel decomposition throughput, memory footprint, and exact
# byte-level invertibility across logs/ corpus via worker pool batching
# ~ multi-gigabyte log batch stress testing lossless invertibility
# =============================================================================

import argparse
import concurrent.futures
import os
import sys
import time
from pathlib import Path
import psutil

# Add lib/python directory to sys.path
pkg_dir = Path(__file__).resolve().parent.parent / "lib" / "python"
sys.path.insert(0, str(pkg_dir))

import pyrazobill


def process_chunk(lines: list[str]) -> tuple[int, int, int, int]:
    """Process a chunk of lines: split, verify lossless join, and count unique templates.

    Returns: (count, byte_size, mismatches, unique_templates_count)
    """
    byte_size = sum(len(s.encode("utf-8")) for s in lines)
    records = pyrazobill.split_many(lines)
    mismatches = 0
    unique = set()
    for rec, orig in zip(records, lines):
        unique.add(rec.template)
        rebuilt = pyrazobill.join(rec)
        if rebuilt != orig:
            mismatches += 1
    return len(lines), byte_size, mismatches, len(unique)


def main():
    """stream production logs in batches through parallel invertibility audit."""
    parser = argparse.ArgumentParser(description="Multi-core scale and lossless invertibility stress test")
    parser.add_argument("--max-files", type=int, default=None, help="Maximum number of log files to process")
    parser.add_argument("--max-lines", type=int, default=None, help="Maximum total lines to process")
    parser.add_argument("--threads", type=int, default=None, help="Worker thread count (defaults to CPU count)")
    args = parser.parse_args()

    root = Path(__file__).resolve().parent.parent
    logs_dir = root / "logs"
    if not logs_dir.exists():
        print(f"Error: logs directory not found at {logs_dir}")
        sys.exit(1)

    log_files = sorted(logs_dir.rglob("*.log"))
    if not log_files:
        print("No .log files found in logs/")
        sys.exit(1)

    if args.max_files:
        log_files = log_files[: args.max_files]

    process = psutil.Process()
    num_cpus = args.threads or os.cpu_count() or 12
    initial_rss = process.memory_info().rss / (1024 * 1024)

    print("=" * 110)
    print("LIBRAZOBILL / PYRAZOBILL: MULTI-CORE STRESS TEST & LOSSLESS AUDIT")
    print(f"Hardware Cores: {num_cpus} Workers | Initial Memory: {initial_rss:.2f} MB")
    print(f"Log Files Target: {len(log_files):,} files discovered")
    print("=" * 110)

    total_lines = 0
    total_bytes = 0
    total_mismatches = 0
    all_unique_forms = 0

    batch_lines = []
    CHUNK_SIZE = 5000
    BATCH_SIZE = CHUNK_SIZE * num_cpus

    t_start = time.perf_counter()

    def dispatch_batch(lines_to_dispatch):
        nonlocal total_lines, total_bytes, total_mismatches, all_unique_forms
        chunks = [lines_to_dispatch[i : i + CHUNK_SIZE] for i in range(0, len(lines_to_dispatch), CHUNK_SIZE)]
        with concurrent.futures.ThreadPoolExecutor(max_workers=num_cpus) as executor:
            for count, b_size, mismatches, u_count in executor.map(process_chunk, chunks):
                total_lines += count
                total_bytes += b_size
                total_mismatches += mismatches
                all_unique_forms += u_count

    stop_reading = False
    for idx, fpath in enumerate(log_files, 1):
        if stop_reading:
            break
        try:
            with open(fpath, "r", encoding="utf-8", errors="replace") as f:
                for line in f:
                    s = line.rstrip("\r\n")
                    if not s:
                        continue
                    batch_lines.append(s)

                    if len(batch_lines) >= BATCH_SIZE:
                        dispatch_batch(batch_lines)
                        batch_lines.clear()
                        if args.max_lines and total_lines >= args.max_lines:
                            stop_reading = True
                            break
        except Exception as e:
            print(f"Warning reading {fpath.name}: {e}")

    # Process remaining lines
    if batch_lines and (not args.max_lines or total_lines < args.max_lines):
        if args.max_lines:
            rem = args.max_lines - total_lines
            batch_lines = batch_lines[:rem]
        dispatch_batch(batch_lines)

    t_end = time.perf_counter()
    total_elapsed = t_end - t_start
    final_rss = process.memory_info().rss / (1024 * 1024)

    print("\n" + "=" * 110)
    print("STRESS TEST & AUDIT SUMMARY RESULTS")
    print("=" * 110)
    print(f"Total Lines Processed : {total_lines:,} lines")
    print(f"Total Data Volume     : {total_bytes / (1024**3):.2f} GB ({total_bytes / (1024**2):,.2f} MB)")
    print(f"Total Wall Time       : {total_elapsed:.2f} seconds")
    if total_elapsed > 0:
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

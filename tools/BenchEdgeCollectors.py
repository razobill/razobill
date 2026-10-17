#!/usr/bin/env python3
# =============================================================================
# edge telemetry forwarder compaction benchmark (OpenTelemetry and Vector)
#
# simulates in-flight edge decomposition, nanosecond per-record latency (p50,
# p99), wire egress compression, and byte-for-byte exact invertibility
# ~ بنحاكي دور مجمع السجلات على الأجهزة الطرفية قبل ما يبعت على الشبكة
# =============================================================================

import sys
import time
import zlib
from pathlib import Path

# Add lib/python to sys.path
pkg_dir = Path(__file__).resolve().parent.parent / "lib" / "python"
sys.path.insert(0, str(pkg_dir))

import pyrazobill


def runEdgeCollectorBenchmark(logPath: str, maxEvents: int = 50000):
    """measure latency, wire reduction, and lossless invertibility on log file.

    inv: every processed record must reconstruct identically via RzJoin
    in: logPath is path to readable text log file; maxEvents > 0
    out: dict of benchmark metrics or None if file missing or empty
    """
    path = Path(logPath)
    if not path.exists():
        print(f"[EDGE BENCH] File {logPath} does not exist.")
        return None

    lines = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            s = line.rstrip("\r\n")
            if s:
                lines.append(s)
                if len(lines) >= maxEvents:
                    break

    n = len(lines)
    if n == 0:
        return None

    print("=" * 100)
    print("RAZOBILL EDGE COLLECTOR BENCHMARK (OPENTELEMETRY & VECTOR COMPACTION)")
    print("=" * 100)
    print(f"Log Corpus        : {path.name} ({path.parent.name})")
    print(f"Total Log Events  : {n:,}")

    rawBytes = sum(len(m.encode("utf-8")) for m in lines)
    print(f"Raw Wire Payload  : {rawBytes:,} bytes ({rawBytes / (1024 * 1024):.2f} MB)")
    print(f"Mean Event Size   : {rawBytes / n:.1f} bytes/record")
    print("-" * 100)

    # 1. Measure decomposition latency distribution
    latenciesNs = []
    decomposedRecords = []
    mismatches = 0

    t0 = time.perf_counter()
    for line in lines:
        tS = time.perf_counter_ns()
        rec = pyrazobill.RzSplit(line)
        tE = time.perf_counter_ns()
        latenciesNs.append(tE - tS)
        decomposedRecords.append((rec.template, rec.ints, rec.strs))
    totalSec = time.perf_counter() - t0

    latenciesNs.sort()
    p50Us = latenciesNs[int(n * 0.50)] / 1000.0
    p90Us = latenciesNs[int(n * 0.90)] / 1000.0
    p99Us = latenciesNs[int(n * 0.99)] / 1000.0
    p999Us = latenciesNs[int(n * 0.999)] / 1000.0
    throughput = n / totalSec

    print("[Phase 1: In-Process Decomposition Latency Overhead]")
    print(f"  - Hot-Path Throughput : {throughput:>10,.0f} records/second")
    print(f"  - Median Latency (p50): {p50Us:>10.2f} microseconds")
    print(f"  - 90th Percentile(p90): {p90Us:>10.2f} microseconds")
    print(f"  - 99th Percentile(p99): {p99Us:>10.2f} microseconds")
    print(f"  - Tail Latency  (p99.9):{p999Us:>10.2f} microseconds")

    # 2. Wire Egress Size Calculation
    # Packed representation: formKey UTF-8 + 8 bytes per integer +
    # UTF-8 string literals
    rawWireGzip = len(zlib.compress("\n".join(lines).encode("utf-8"), 6))

    # Razobill wire representation
    # Unique forms dictionary + references, plus integer and string stripes
    uniqueForms = {}
    formRefBytes = 0
    intSlotBytes = 0
    strSlotBytes = 0

    for form, ints, strs in decomposedRecords:
        if form not in uniqueForms:
            uniqueForms[form] = len(uniqueForms)
        formRefBytes += 4  # 32-bit form ID per row
        intSlotBytes += len(ints) * 8  # 64-bit int
        strSlotBytes += sum(len(s.encode("utf-8")) for s in strs)

    dictFormBytes = sum(len(f.encode("utf-8")) for f in uniqueForms.keys())

    import struct
    import duckdb

    # Columnar bytes measurement via Parquet ZSTD
    # (standard Vector / OTel S3 sink codec)
    con = duckdb.connect()
    con.execute("CREATE TABLE edge_stream (form VARCHAR, ints BIGINT[], strs VARCHAR[]);")
    con.executemany("INSERT INTO edge_stream VALUES (?, ?, ?);", decomposedRecords)

    pqFile = Path("build") / f"edge_{path.stem}.parquet"
    pqFile.parent.mkdir(parents=True, exist_ok=True)
    con.execute(f"COPY edge_stream TO '{pqFile.as_posix()}' (FORMAT PARQUET, COMPRESSION ZSTD);")
    razobillParquetBytes = pqFile.stat().st_size

    # Also compute packed binary bytes
    # (form dictionary + packed ints + string views)
    packedIntBytes = b"".join(struct.pack("<q", int(i)) for r in decomposedRecords for i in r[1])
    columnarBlob = (
        "".join(r[0] for r in decomposedRecords).encode("utf-8")
        + packedIntBytes
        + "".join(s for r in decomposedRecords for s in r[2]).encode("utf-8")
    )
    razobillGzipWire = len(zlib.compress(columnarBlob, 6))

    parquetSavedPct = (1.0 - (razobillParquetBytes / rawBytes)) * 100.0
    gzipSavedPct = (1.0 - (razobillGzipWire / rawWireGzip)) * 100.0

    print("\n[Phase 2: Network Wire & Cloud Egress Reduction]")
    print(f"  - Raw Text Wire Payload : {rawBytes:>12,} bytes (100.0%)")
    print(f"  - Raw Gzip Wire Stream  : {rawWireGzip:>12,} bytes (Baseline compressed)")
    print(f"  - Unique Forms Dictionary: {dictFormBytes:>12,} bytes ({len(uniqueForms):,} unique forms)")
    print(f"  - Razobill Columnar Gzip: {razobillGzipWire:>12,} bytes [Compressed Wire Reduced by {gzipSavedPct:.2f}%]")
    print(
        f"  - Razobill Parquet ZSTD : {razobillParquetBytes:>12,} bytes [Cloud Egress Reduced by {parquetSavedPct:.2f}%]"
    )

    # 3. Verify Exact Lossless Invertibility
    for (form, ints, strs), rawLine in zip(decomposedRecords, lines):
        rebuilt = pyrazobill.RzJoin(form, ints, strs)
        if rebuilt != rawLine:
            mismatches += 1

    print("\n[Phase 3: Mathematical Invertibility & Lossless Guarantee]")
    print(f"  - Events Evaluated      : {n:,}")
    print(f"  - Bit-Level Mismatches  : {mismatches}")
    print(f"  - Invertibility Proof   : {'100.000% EXACT (Zero Drift)' if mismatches == 0 else 'FAILED'}")
    print("=" * 100)

    return {
        "dataset": path.name,
        "n": n,
        "tput": throughput,
        "p50": p50Us,
        "p99": p99Us,
        "rawBytes": rawBytes,
        "rzParquet": razobillParquetBytes,
        "parquetSavedPct": parquetSavedPct,
        "gzipSavedPct": gzipSavedPct,
        "mismatches": mismatches,
    }


def main():
    root = Path(__file__).resolve().parent.parent
    candidates = [
        root / "logs/SSH/SSH.log",
        root / "logs/Linux/Linux.log",
        root / "logs/Proxifier/Proxifier.log",
    ]
    results = []
    for c in candidates:
        if c.exists():
            res = runEdgeCollectorBenchmark(str(c), maxEvents=30000)
            if res:
                results.append(res)

    print("\n" + "=" * 110)
    print("CONSOLIDATED EDGE COLLECTOR SUMMARY")
    print("=" * 110)
    print(
        f"{'LOG DATASET':<15} | {'EVENTS':<8} | {'THROUGHPUT':<14} | {'p50 LATENCY':<12} | {'p99 LATENCY':<12} | {'EGRESS SAVINGS':<15} | {'LOSSLESS'}"
    )
    print("-" * 110)
    for r in results:
        print(
            f"{r['dataset']:<15} | {r['n']:<8,} | {r['tput']:>10,.0f} evt/s | {r['p50']:>10.2f} us | {r['p99']:>10.2f} us | {r['parquetSavedPct']:>13.2f}% | {'100.000%' if r['mismatches'] == 0 else 'FAIL'}"
        )
    print("=" * 110)


if __name__ == "__main__":
    main()

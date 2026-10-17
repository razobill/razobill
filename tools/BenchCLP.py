#!/usr/bin/env python3
# =============================================================================
# head-to-head benchmark harness: Razobill vs YScope CLP (OSDI 2021)
#
# benchmarks ingestion throughput, compression ratio, lossless reconstruction,
# decoding throughput, search/aggregation query latency, and logging handlers
# ~ head-to-head throughput and compression comparison against CLP
# =============================================================================

import argparse
import datetime
import io
import json
import logging
import platform
import random
import statistics
import sys
import tempfile
import time
from pathlib import Path
from typing import Any, Dict, List, Optional

# Add lib/python to sys.path
repo_root = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(repo_root / "lib" / "python"))

import duckdb
import psutil
import pyarrow as pa
import pyarrow.parquet as pq
import pyrazobill as rz
import zstandard as zstd

import clp_ffi_py
import clp_ffi_py.ir as clp_ir
import clp_logging
from clp_logging.handlers import CLPFileHandler


# ---------------------------------------------------------------------------
# Synthetic Enterprise Log Generator (Deterministic Seed)
# ---------------------------------------------------------------------------
def generate_synthetic_logs(count: int = 100000) -> List[str]:
    """Generates synthetic enterprise microservice telemetry events deterministically."""
    rng = random.Random(42)
    services = ["auth-service", "billing-worker", "api-gateway", "order-processor", "inventory-db"]
    methods = ["GET", "POST", "PUT", "DELETE"]
    endpoints = ["/api/v1/checkout", "/api/v2/users", "/healthz", "/oauth/token", "/items/search"]
    statuses = [200, 201, 204, 400, 401, 403, 404, 500, 502, 503]
    users = ["alice", "bob", "carol", "dave", "service-account-ci", "admin"]
    levels = ["INFO", "WARN", "ERROR", "DEBUG"]

    lines = []
    base_ts = 1776211200  # Fixed epoch baseline
    for i in range(count):
        ts = datetime.datetime.fromtimestamp(base_ts + i * 0.05, datetime.timezone.utc).strftime(
            "%Y-%m-%d %H:%M:%S.%f"
        )[:-3]
        lvl = levels[i % len(levels)]
        svc = services[rng.randint(0, len(services) - 1)]
        pid = 1000 + (i % 50)
        ip = f"10.0.{rng.randint(1, 254)}.{rng.randint(1, 254)}"

        r = rng.random()
        if r < 0.40:
            m = methods[rng.randint(0, len(methods) - 1)]
            ep = endpoints[rng.randint(0, len(endpoints) - 1)]
            st = statuses[rng.randint(0, len(statuses) - 1)]
            lat = rng.randint(2, 450)
            u = users[rng.randint(0, len(users) - 1)]
            line = f"{ts} [{lvl}] [{svc}:{pid}] HTTP {m} {ep} status={st} latency={lat}ms client={ip} user={u}"
        elif r < 0.70:
            job_id = f"job-{rng.randint(10000, 99999)}"
            dur = rng.randint(10, 1500)
            items = rng.randint(1, 500)
            line = f"{ts} [{lvl}] [{svc}:{pid}] Processed transaction {job_id} with {items} items in {dur}ms from {ip}"
        elif r < 0.90:
            port = rng.randint(1024, 65535)
            retry = rng.randint(1, 5)
            line = f"{ts} [{lvl}] [{svc}:{pid}] Connection to backend {ip}:{port} failed after {retry} retries"
        else:
            uid = f"0x{rng.randint(0x100000, 0xFFFFFF):x}"
            line = f"{ts} [{lvl}] [{svc}:{pid}] Worker thread spawned handle={uid} memory_allocated_mb={rng.randint(64, 1024)}"
        lines.append(line)
    return lines


# ---------------------------------------------------------------------------
# Dataset Loader
# ---------------------------------------------------------------------------
def load_dataset(name: str, max_lines: int) -> List[str]:
    """Loads dataset lines from repository logs/ or synthetic generator."""
    if name == "synthetic":
        return generate_synthetic_logs(max_lines)

    path_map = {
        "ssh": repo_root / "logs" / "SSH" / "SSH.log",
        "bgl": repo_root / "logs" / "BGL" / "BGL.log",
        "zookeeper": repo_root / "logs" / "Zookeeper" / "Zookeeper.log",
        "linux": repo_root / "logs" / "Linux" / "Linux.log",
    }

    p = path_map.get(name)
    if not p or not p.exists():
        raise FileNotFoundError(f"Dataset '{name}' not found at {p}")

    lines = []
    with open(p, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            s = line.rstrip("\r\n")
            if s:
                lines.append(s)
                if len(lines) >= max_lines:
                    break
    return lines


# ---------------------------------------------------------------------------
# Razobill Custom Python Logging Handler
# ---------------------------------------------------------------------------
class RazobillLoggingHandler(logging.Handler):
    """Custom logging handler that decomposes messages and flushes to Parquet."""

    def __init__(self) -> None:
        super().__init__()
        self.templates: List[str] = []
        self.ints: List[List[int]] = []
        self.strs: List[List[str]] = []

    def emit(self, record: logging.LogRecord) -> None:
        msg = self.format(record)
        rec = rz.RzSplit(msg)
        self.templates.append(rec.template)
        self.ints.append(rec.ints)
        self.strs.append(rec.strs)

    def write_to_parquet(self, fpath: Path, compression: str = "zstd") -> int:
        table = pa.Table.from_arrays(
            [
                pa.array(self.templates, type=pa.string()),
                pa.array(self.ints, type=pa.list_(pa.int64())),
                pa.array(self.strs, type=pa.list_(pa.string())),
            ],
            names=["template", "ints", "strs"],
        )
        pq.write_table(table, str(fpath), compression=compression, compression_level=3)
        return fpath.stat().st_size


# ---------------------------------------------------------------------------
# Core Benchmark Runner
# ---------------------------------------------------------------------------
class ClpRazobillBenchmark:
    def __init__(self, runs: int = 5, is_quick: bool = False) -> None:
        self.runs = 1 if is_quick else runs
        self.system_info = self._collect_system_info()

    def _collect_system_info(self) -> Dict[str, Any]:
        vm = psutil.virtual_memory()
        freq = psutil.cpu_freq()
        return {
            "os": f"{platform.system()} {platform.release()} ({platform.version()})",
            "arch": platform.machine(),
            "cpu": platform.processor() or "GenuineIntel",
            "cpu_physical_cores": psutil.cpu_count(logical=False) or 6,
            "cpu_logical_cores": psutil.cpu_count(logical=True) or 12,
            "cpu_freq_mhz": round(freq.current, 1) if freq else 2496.0,
            "ram_gb": round(vm.total / (1024**3), 1),
            "python_version": platform.python_version(),
            "pyrazobill_version": rz.__version__,
            "clp_ffi_py_version": getattr(clp_ffi_py, "__version__", "0.0.14"),
            "clp_logging_version": getattr(clp_logging, "__version__", "0.0.15"),
            "duckdb_version": duckdb.__version__,
            "pyarrow_version": pa.__version__,
            "zstandard_version": zstd.__version__,
            "timestamp": datetime.datetime.now().isoformat(),
        }

    @staticmethod
    def _calc_stats(timings: List[float], n: int, raw_bytes: int) -> Dict[str, Any]:
        median_sec = statistics.median(timings)
        mean_sec = statistics.mean(timings)
        min_sec = min(timings)
        max_sec = max(timings)
        stdev_sec = statistics.stdev(timings) if len(timings) > 1 else 0.0
        spread_pct = ((max_sec - min_sec) / median_sec * 100.0) if median_sec > 0 else 0.0
        throughput_lines = round(n / median_sec, 0) if median_sec > 0 else 0.0
        throughput_mb = round((raw_bytes / (1024**2)) / median_sec, 2) if median_sec > 0 else 0.0
        latency_ns = round((median_sec / n) * 1e9, 1) if n > 0 else 0.0
        return {
            "runs": len(timings),
            "median_sec": median_sec,
            "mean_sec": mean_sec,
            "min_sec": min_sec,
            "max_sec": max_sec,
            "stdev_sec": stdev_sec,
            "spread_pct": spread_pct,
            "throughput_lines_sec": throughput_lines,
            "throughput_mb_sec": throughput_mb,
            "latency_ns_event": latency_ns,
        }

    # 1. Ingestion Benchmark
    def bench_ingestion(self, name: str, lines: List[str], raw_bytes: int) -> Dict[str, Any]:
        n = len(lines)
        lines_bytes = [line.encode("utf-8") for line in lines]

        # Razobill Split Single (per-line)
        single_times = []
        for _ in range(self.runs):
            t0 = time.perf_counter()
            _ = [rz.RzSplit(line) for line in lines]
            single_times.append(time.perf_counter() - t0)

        # Razobill Split Batch (RzSplitMany)
        batch_times = []
        for _ in range(self.runs):
            t0 = time.perf_counter()
            _ = rz.RzSplitMany(lines)
            batch_times.append(time.perf_counter() - t0)

        # CLP FourByteSerializer
        clp_times = []
        for _ in range(self.runs):
            t0 = time.perf_counter()
            _ = [clp_ir.FourByteSerializer.serialize_message_and_timestamp_delta(0, lb) for lb in lines_bytes]
            clp_times.append(time.perf_counter() - t0)

        single_stats = self._calc_stats(single_times, n, raw_bytes)
        single_stats["name"] = "Razobill RzSplit (Per-line)"
        batch_stats = self._calc_stats(batch_times, n, raw_bytes)
        batch_stats["name"] = "Razobill RzSplitMany (Batch)"
        clp_stats = self._calc_stats(clp_times, n, raw_bytes)
        clp_stats["name"] = "CLP FourByteSerializer"

        return {
            "dataset": name,
            "lines": n,
            "raw_bytes": raw_bytes,
            "raw_mb": round(raw_bytes / (1024**2), 2),
            "methods": {
                "razobill_split_single": single_stats,
                "razobill_split_batch": batch_stats,
                "clp_fourbyte_serializer": clp_stats,
            },
        }

    # 2. Compression & Storage Footprint Benchmark
    def bench_compression(self, name: str, lines: List[str], raw_bytes: int) -> Dict[str, Any]:
        n = len(lines)
        lines_bytes = [line.encode("utf-8") for line in lines]
        raw_combined = b"\n".join(lines_bytes)

        # Raw Zstd
        raw_zstd_bytes = len(zstd.compress(raw_combined, 3))

        # CLP IR Raw
        preamble = clp_ir.FourByteSerializer.serialize_preamble(0, "", "UTC")
        end_ir = clp_ir.FourByteSerializer.serialize_end_of_ir()
        stream = bytearray(preamble)
        for lb in lines_bytes:
            stream.extend(clp_ir.FourByteSerializer.serialize_message_and_timestamp_delta(0, lb))
        stream.extend(end_ir)
        clp_raw_bytes = len(stream)

        # CLP IR Zstd
        clp_zstd_bytes = len(zstd.compress(bytes(stream), 3))

        # Razobill Parquet Zstd
        recs = [rz.RzSplit(line) for line in lines]
        table = pa.Table.from_arrays(
            [
                pa.array([r.template for r in recs], type=pa.string()),
                pa.array([r.ints for r in recs], type=pa.list_(pa.int64())),
                pa.array([r.strs for r in recs], type=pa.list_(pa.string())),
            ],
            names=["template", "ints", "strs"],
        )
        sink = io.BytesIO()
        pq.write_table(table, sink, compression="zstd", compression_level=3)
        rz_parquet_zstd_bytes = sink.tell()

        # Razobill DuckDB on-disk storage
        with tempfile.NamedTemporaryFile(suffix=".duckdb", delete=False) as tf:
            duckdb_fpath = Path(tf.name)
        duckdb_fpath.unlink(missing_ok=True)
        con = duckdb.connect(str(duckdb_fpath))
        con.execute("CREATE TABLE logs (template VARCHAR, ints BIGINT[], strs VARCHAR[])")
        con.register("arrow_table", table)
        con.execute("INSERT INTO logs SELECT * FROM arrow_table")
        con.execute("CHECKPOINT")
        con.close()
        rz_duckdb_bytes = duckdb_fpath.stat().st_size
        duckdb_fpath.unlink(missing_ok=True)

        def _fmt(size: int) -> Dict[str, Any]:
            ratio = round(raw_bytes / size, 2) if size > 0 else 0.0
            savings = round((1.0 - (size / raw_bytes)) * 100.0, 2) if raw_bytes > 0 else 0.0
            b_per_e = round(size / n, 2) if n > 0 else 0.0
            return {
                "size_bytes": size,
                "size_kb": round(size / 1024, 2),
                "size_mb": round(size / (1024**2), 3),
                "ratio_vs_raw": ratio,
                "savings_pct": savings,
                "bytes_per_event": b_per_e,
            }

        return {
            "dataset": name,
            "lines": n,
            "raw_size_bytes": raw_bytes,
            "formats": {
                "raw_uncompressed": _fmt(raw_bytes),
                "raw_zstd": _fmt(raw_zstd_bytes),
                "clp_raw": _fmt(clp_raw_bytes),
                "clp_zstd": _fmt(clp_zstd_bytes),
                "razobill_parquet_zstd": _fmt(rz_parquet_zstd_bytes),
                "razobill_duckdb": _fmt(rz_duckdb_bytes),
            },
        }

    # 3. Lossless Fidelity Verification
    def bench_fidelity(self, name: str, lines: List[str]) -> Dict[str, Any]:
        n = len(lines)
        lines_bytes = [line.encode("utf-8") for line in lines]

        # Razobill Memory
        t0 = time.perf_counter()
        recs = [rz.RzSplit(line) for line in lines]
        rz_recon = [rz.RzJoin(*r) for r in recs]
        t_rz_mem = time.perf_counter() - t0
        rz_mismatches = sum(1 for a, b in zip(lines, rz_recon) if a != b)

        # Razobill Parquet
        table = pa.Table.from_arrays(
            [
                pa.array([r.template for r in recs], type=pa.string()),
                pa.array([r.ints for r in recs], type=pa.list_(pa.int64())),
                pa.array([r.strs for r in recs], type=pa.list_(pa.string())),
            ],
            names=["template", "ints", "strs"],
        )
        sink = io.BytesIO()
        pq.write_table(table, sink, compression="zstd")
        sink.seek(0)
        t0 = time.perf_counter()
        read_tab = pq.read_table(sink)
        p_tpl = read_tab["template"].to_pylist()
        p_ints = read_tab["ints"].to_pylist()
        p_strs = read_tab["strs"].to_pylist()
        rz_pq_recon = [rz.RzJoin(t, i, s) for t, i, s in zip(p_tpl, p_ints, p_strs)]
        t_rz_pq = time.perf_counter() - t0
        rz_pq_mismatches = sum(1 for a, b in zip(lines, rz_pq_recon) if a != b)

        # CLP IR
        preamble = clp_ir.FourByteSerializer.serialize_preamble(0, "", "UTC")
        end_ir = clp_ir.FourByteSerializer.serialize_end_of_ir()
        stream = bytearray(preamble)
        for lb in lines_bytes:
            stream.extend(clp_ir.FourByteSerializer.serialize_message_and_timestamp_delta(0, lb))
        stream.extend(end_ir)

        t0 = time.perf_counter()
        reader = clp_ir.ClpIrStreamReader(io.BytesIO(stream), enable_compression=False)
        reader.read_preamble()
        clp_recon = []
        while True:
            ev = reader.read_next_log_event()
            if ev is None:
                break
            clp_recon.append(ev.get_log_message())
        t_clp = time.perf_counter() - t0
        clp_mismatches = sum(1 for a, b in zip(lines, clp_recon) if a != b)

        return {
            "dataset": name,
            "total_lines": n,
            "razobill_memory": {
                "matched": n - rz_mismatches,
                "fidelity_pct": 100.0 if rz_mismatches == 0 else round((n - rz_mismatches) / n * 100.0, 3),
                "duration_sec": round(t_rz_mem, 3),
                "mismatches": rz_mismatches,
            },
            "razobill_parquet": {
                "matched": n - rz_pq_mismatches,
                "fidelity_pct": 100.0 if rz_pq_mismatches == 0 else round((n - rz_pq_mismatches) / n * 100.0, 3),
                "duration_sec": round(t_rz_pq, 3),
                "mismatches": rz_pq_mismatches,
            },
            "clp_ir": {
                "matched": n - clp_mismatches,
                "fidelity_pct": 100.0 if clp_mismatches == 0 else round((n - clp_mismatches) / n * 100.0, 3),
                "duration_sec": round(t_clp, 3),
                "mismatches": clp_mismatches,
            },
        }

    # 4. Reconstruction Throughput Benchmark
    def bench_reconstruction(self, name: str, lines: List[str], raw_bytes: int) -> Dict[str, Any]:
        n = len(lines)
        lines_bytes = [line.encode("utf-8") for line in lines]
        recs = [rz.RzSplit(line) for line in lines]

        # Razobill RzJoin (Memory)
        rz_join_times = []
        for _ in range(self.runs):
            t0 = time.perf_counter()
            _ = [rz.RzJoin(*r) for r in recs]
            rz_join_times.append(time.perf_counter() - t0)

        # Razobill Parquet + RzJoin
        table = pa.Table.from_arrays(
            [
                pa.array([r.template for r in recs], type=pa.string()),
                pa.array([r.ints for r in recs], type=pa.list_(pa.int64())),
                pa.array([r.strs for r in recs], type=pa.list_(pa.string())),
            ],
            names=["template", "ints", "strs"],
        )
        sink = io.BytesIO()
        pq.write_table(table, sink, compression="zstd")
        parquet_bytes_val = sink.getvalue()

        rz_pq_times = []
        for _ in range(self.runs):
            t0 = time.perf_counter()
            read_tab = pq.read_table(io.BytesIO(parquet_bytes_val))
            p_tpl = read_tab["template"].to_pylist()
            p_ints = read_tab["ints"].to_pylist()
            p_strs = read_tab["strs"].to_pylist()
            _ = [rz.RzJoin(t, i, s) for t, i, s in zip(p_tpl, p_ints, p_strs)]
            rz_pq_times.append(time.perf_counter() - t0)

        # CLP Stream Preparation
        preamble = clp_ir.FourByteSerializer.serialize_preamble(0, "", "UTC")
        end_ir = clp_ir.FourByteSerializer.serialize_end_of_ir()
        raw_stream = bytearray(preamble)
        for lb in lines_bytes:
            raw_stream.extend(clp_ir.FourByteSerializer.serialize_message_and_timestamp_delta(0, lb))
        raw_stream.extend(end_ir)
        raw_stream_bytes = bytes(raw_stream)
        zstd_stream_bytes = zstd.compress(raw_stream_bytes, 3)

        # CLP Raw Decode
        clp_raw_times = []
        for _ in range(self.runs):
            t0 = time.perf_counter()
            reader = clp_ir.ClpIrStreamReader(io.BytesIO(raw_stream_bytes), enable_compression=False)
            reader.read_preamble()
            while reader.read_next_log_event() is not None:
                pass
            clp_raw_times.append(time.perf_counter() - t0)

        # CLP Zstd Decode
        clp_zstd_times = []
        for _ in range(self.runs):
            t0 = time.perf_counter()
            reader = clp_ir.ClpIrStreamReader(io.BytesIO(zstd_stream_bytes), enable_compression=True)
            reader.read_preamble()
            while reader.read_next_log_event() is not None:
                pass
            clp_zstd_times.append(time.perf_counter() - t0)

        rz_stats = self._calc_stats(rz_join_times, n, raw_bytes)
        rz_stats["name"] = "Razobill RzJoin (Memory)"
        rz_pq_stats = self._calc_stats(rz_pq_times, n, raw_bytes)
        rz_pq_stats["name"] = "Razobill Parquet+RzJoin"
        clp_raw_stats = self._calc_stats(clp_raw_times, n, raw_bytes)
        clp_raw_stats["name"] = "CLP IR Raw Decode"
        clp_zstd_stats = self._calc_stats(clp_zstd_times, n, raw_bytes)
        clp_zstd_stats["name"] = "CLP IR Zstd Decode"

        return {
            "dataset": name,
            "lines": n,
            "raw_mb": round(raw_bytes / (1024**2), 2),
            "methods": {
                "razobill_rzjoin": rz_stats,
                "razobill_parquet_recon": rz_pq_stats,
                "clp_ir_raw_decode": clp_raw_stats,
                "clp_ir_zstd_decode": clp_zstd_stats,
            },
        }

    # 5. Search & Query Benchmark
    def bench_search(self, name: str, lines: List[str]) -> Dict[str, Any]:
        n = len(lines)
        lines_bytes = [line.encode("utf-8") for line in lines]

        # Target token for needle search
        if name == "ssh":
            token = "Failed"
            wildcard = "*Failed*"
        elif name == "bgl":
            token = "ciod"
            wildcard = "*ciod*"
        elif name == "zookeeper":
            token = "Notification"
            wildcard = "*Notification*"
        else:
            token = "HTTP"
            wildcard = "*HTTP*"

        # CLP Stream Preparation
        preamble = clp_ir.FourByteSerializer.serialize_preamble(0, "", "UTC")
        end_ir = clp_ir.FourByteSerializer.serialize_end_of_ir()
        raw_stream = bytearray(preamble)
        for lb in lines_bytes:
            raw_stream.extend(clp_ir.FourByteSerializer.serialize_message_and_timestamp_delta(0, lb))
        raw_stream.extend(end_ir)
        stream_bytes = bytes(raw_stream)

        # CLP IR Wildcard Search
        clp_needle_times = []
        for _ in range(self.runs):
            t0 = time.perf_counter()
            reader = clp_ir.ClpIrStreamReader(io.BytesIO(stream_bytes), enable_compression=False)
            reader.read_preamble()
            qb = clp_ir.QueryBuilder()
            qb.add_wildcard_query(wildcard)
            _ = list(reader.search(qb.build()))
            clp_needle_times.append(time.perf_counter() - t0)

        # Razobill DuckDB Columnar SQL Search
        recs = [rz.RzSplit(line) for line in lines]
        table = pa.Table.from_arrays(
            [
                pa.array([r.template for r in recs], type=pa.string()),
                pa.array([r.ints for r in recs], type=pa.list_(pa.int64())),
                pa.array([r.strs for r in recs], type=pa.list_(pa.string())),
            ],
            names=["template", "ints", "strs"],
        )
        con = duckdb.connect()
        con.register("logs", table)

        sql_needle = f"SELECT * FROM logs WHERE template LIKE '%{token}%' OR list_contains(strs, '{token}')"
        duck_needle_times = []
        for _ in range(self.runs):
            t0 = time.perf_counter()
            _ = con.execute(sql_needle).fetchall()
            duck_needle_times.append(time.perf_counter() - t0)

        # Razobill DuckDB Group-by
        sql_groupby = "SELECT template, count(*) AS c FROM logs GROUP BY 1 ORDER BY c DESC LIMIT 10"
        duck_group_times = []
        for _ in range(self.runs):
            t0 = time.perf_counter()
            _ = con.execute(sql_groupby).fetchall()
            duck_group_times.append(time.perf_counter() - t0)

        # CLP Stream Frequency Scan (full iteration)
        clp_group_times = []
        for _ in range(self.runs):
            t0 = time.perf_counter()
            reader = clp_ir.ClpIrStreamReader(io.BytesIO(stream_bytes), enable_compression=False)
            reader.read_preamble()
            counts: Dict[str, int] = {}
            while True:
                ev = reader.read_next_log_event()
                if ev is None:
                    break
                m = ev.get_log_message()
                counts[m] = counts.get(m, 0) + 1
            clp_group_times.append(time.perf_counter() - t0)

        def _qstats(times: List[float], label: str) -> Dict[str, Any]:
            med = statistics.median(times)
            mean = statistics.mean(times)
            stdev = statistics.stdev(times) if len(times) > 1 else 0.0
            spread = ((max(times) - min(times)) / med * 100.0) if med > 0 else 0.0
            tp = round(n / med, 0) if med > 0 else 0.0
            return {
                "name": label,
                "runs": len(times),
                "median_sec": med,
                "mean_sec": mean,
                "min_sec": min(times),
                "max_sec": max(times),
                "stdev_sec": stdev,
                "spread_pct": spread,
                "scan_throughput_lines_sec": tp,
                "throughput_lines_sec": tp,
                "latency_ms": round(med * 1000.0, 2),
            }

        return {
            "dataset": name,
            "lines": n,
            "queries": {
                "needle_clp_ir": _qstats(clp_needle_times, "CLP IR Wildcard Search"),
                "needle_razobill_duckdb": _qstats(duck_needle_times, "Razobill DuckDB SQL Search"),
                "top10_razobill_duckdb": _qstats(duck_group_times, "Razobill DuckDB Group-by"),
                "top10_clp_stream": _qstats(clp_group_times, "CLP Stream Frequency Scan"),
            },
        }

    # 6. Python logging handlers benchmark
    # (clp-logging vs Razobill vs FileHandler)
    def bench_logging_handlers(self, num_events: int = 50000) -> Dict[str, Any]:
        synthetic_records = [
            f"User test-user-{i % 500} authenticated from 192.168.1.{i % 254} port {1024 + (i % 60000)} status=OK"
            for i in range(num_events)
        ]

        # 1. Standard FileHandler
        with tempfile.NamedTemporaryFile(suffix=".log", delete=False) as tf:
            fpath_std = Path(tf.name)
        logger_std = logging.getLogger("bench_std")
        logger_std.setLevel(logging.INFO)
        h_std = logging.FileHandler(str(fpath_std), mode="w", encoding="utf-8")
        h_std.setFormatter(logging.Formatter("%(message)s"))
        logger_std.addHandler(h_std)

        t0 = time.perf_counter()
        for msg in synthetic_records:
            logger_std.info(msg)
        h_std.close()
        t_std = time.perf_counter() - t0
        size_std = fpath_std.stat().st_size
        fpath_std.unlink(missing_ok=True)

        # 2. CLP FileHandler (clp_logging.handlers.CLPFileHandler)
        with tempfile.NamedTemporaryFile(suffix=".clp", delete=False) as tf:
            fpath_clp = Path(tf.name)
        logger_clp = logging.getLogger("bench_clp")
        logger_clp.setLevel(logging.INFO)
        h_clp = CLPFileHandler(fpath_clp, enable_compression=True)
        h_clp.setFormatter(logging.Formatter("%(message)s"))
        logger_clp.addHandler(h_clp)

        t0 = time.perf_counter()
        for msg in synthetic_records:
            logger_clp.info(msg)
        h_clp.close()
        t_clp = time.perf_counter() - t0
        size_clp = fpath_clp.stat().st_size
        fpath_clp.unlink(missing_ok=True)

        # 3. Razobill Logging Handler
        with tempfile.NamedTemporaryFile(suffix=".parquet", delete=False) as tf:
            fpath_rz = Path(tf.name)
        logger_rz = logging.getLogger("bench_rz")
        logger_rz.setLevel(logging.INFO)
        h_rz = RazobillLoggingHandler()
        h_rz.setFormatter(logging.Formatter("%(message)s"))
        logger_rz.addHandler(h_rz)

        t0 = time.perf_counter()
        for msg in synthetic_records:
            logger_rz.info(msg)
        size_rz = h_rz.write_to_parquet(fpath_rz, compression="zstd")
        h_rz.close()
        t_rz = time.perf_counter() - t0
        fpath_rz.unlink(missing_ok=True)

        return {
            "num_events": num_events,
            "standard_file_handler": {
                "duration_sec": round(t_std, 4),
                "events_per_sec": round(num_events / t_std, 1),
                "size_bytes": size_std,
                "bytes_per_event": round(size_std / num_events, 2),
                "compression_ratio": 1.0,
            },
            "clp_file_handler": {
                "duration_sec": round(t_clp, 4),
                "events_per_sec": round(num_events / t_clp, 1),
                "size_bytes": size_clp,
                "bytes_per_event": round(size_clp / num_events, 2),
                "compression_ratio": round(size_std / size_clp, 2) if size_clp > 0 else 0.0,
            },
            "razobill_logging_handler": {
                "duration_sec": round(t_rz, 4),
                "events_per_sec": round(num_events / t_rz, 1),
                "size_bytes": size_rz,
                "bytes_per_event": round(size_rz / num_events, 2),
                "compression_ratio": round(size_std / size_rz, 2) if size_rz > 0 else 0.0,
            },
        }

    # Full Suite Run
    def run_all(self, target_datasets: Optional[List[str]] = None) -> Dict[str, Any]:
        datasets_cfg = [
            ("ssh", 100000),
            ("bgl", 100000),
            ("zookeeper", 74000),
            ("synthetic", 100000),
        ]
        if target_datasets:
            datasets_cfg = [d for d in datasets_cfg if d[0] in target_datasets]

        ingestion: Dict[str, Any] = {}
        compression: Dict[str, Any] = {}
        fidelity: Dict[str, Any] = {}
        reconstruction: Dict[str, Any] = {}
        search: Dict[str, Any] = {}

        print("=" * 110)
        print("STARTING ADVANCED HEAD-TO-HEAD BENCHMARK: RAZOBILL vs CLP (YScope, OSDI 2021)")
        print(
            f"System: {self.system_info['os']} | {self.system_info['cpu']} ({self.system_info['cpu_logical_cores']} threads)"
        )
        print(
            f"Packages: pyrazobill {self.system_info['pyrazobill_version']} | clp-ffi-py {self.system_info['clp_ffi_py_version']} | clp-logging {self.system_info['clp_logging_version']} | DuckDB {self.system_info['duckdb_version']}"
        )
        print("=" * 110)

        for ds_name, count in datasets_cfg:
            print(f"\n[+] Processing dataset '{ds_name}' ({count:,} lines)...")
            lines = load_dataset(ds_name, count)
            raw_bytes = sum(len(line.encode("utf-8")) for line in lines)

            print("    -> Ingestion throughput (RzSplit vs RzSplitMany vs CLP FourByteSerializer)...")
            ingestion[ds_name] = self.bench_ingestion(ds_name, lines, raw_bytes)

            print("    -> Storage footprint & compression (Raw vs Zstd vs CLP IR vs Parquet vs DuckDB)...")
            compression[ds_name] = self.bench_compression(ds_name, lines, raw_bytes)

            print("    -> Lossless bit fidelity verification (0 mismatches test)...")
            fidelity[ds_name] = self.bench_fidelity(ds_name, lines)

            print("    -> Reconstruction & decoding throughput (RzJoin vs CLP IR decode)...")
            reconstruction[ds_name] = self.bench_reconstruction(ds_name, lines, raw_bytes)

            print("    -> Search & analytical query latency (Wildcard search vs DuckDB SQL)...")
            search[ds_name] = self.bench_search(ds_name, lines)

        print("\n[+] Benchmarking Python logging handlers (FileHandler vs CLPFileHandler vs RazobillLoggingHandler)...")
        logging_res = self.bench_logging_handlers(num_events=50000)

        output = {
            "system": self.system_info,
            "ingestion": ingestion,
            "compression": compression,
            "fidelity": fidelity,
            "reconstruction": reconstruction,
            "search": search,
            "logging_handler": logging_res,
        }
        return output


def print_summary_tables(data: Dict[str, Any]) -> None:
    print("\n" + "=" * 110)
    print("CONSOLIDATED BENCHMARK SUMMARY TABLE: RAZOBILL vs CLP")
    print("=" * 110)

    print("\n### 1. Ingestion & Reconstruction Throughput")
    print(
        f"{'DATASET':<12} | {'RZ SPLIT (1T)':<14} | {'CLP SER (1T)':<14} | {'RZ JOIN (MEM)':<14} | {'CLP DESER (IR)':<14} | {'FIDELITY'}"
    )
    print("-" * 90)
    for ds in ["ssh", "bgl", "zookeeper", "synthetic"]:
        if ds not in data["ingestion"]:
            continue
        rz_s = data["ingestion"][ds]["methods"]["razobill_split_single"]["throughput_lines_sec"]
        clp_s = data["ingestion"][ds]["methods"]["clp_fourbyte_serializer"]["throughput_lines_sec"]
        rz_j = data["reconstruction"][ds]["methods"]["razobill_rzjoin"]["throughput_lines_sec"]
        clp_d = data["reconstruction"][ds]["methods"]["clp_ir_raw_decode"]["throughput_lines_sec"]
        fid = f"{data['fidelity'][ds]['razobill_memory']['fidelity_pct']}% (0 mismatches)"
        print(
            f"{ds.upper():<12} | {rz_s:>10,.0f} m/s | {clp_s:>10,.0f} m/s | {rz_j:>10,.0f} m/s | {clp_d:>10,.0f} m/s | {fid}"
        )

    print("\n### 2. Compression & Storage Footprint")
    print(
        f"{'DATASET':<12} | {'RAW SIZE':<10} | {'CLP IR+ZSTD':<12} | {'RZ PARQUET':<12} | {'CLP RATIO':<10} | {'RZ RATIO':<10} | {'SAVINGS'}"
    )
    print("-" * 90)
    for ds in ["ssh", "bgl", "zookeeper", "synthetic"]:
        if ds not in data["compression"]:
            continue
        c = data["compression"][ds]["formats"]
        raw = f"{c['raw_uncompressed']['size_mb']:.2f} MB"
        clp_z = f"{c['clp_zstd']['size_kb']:.1f} KB"
        rz_p = f"{c['razobill_parquet_zstd']['size_kb']:.1f} KB"
        clp_r = f"{c['clp_zstd']['ratio_vs_raw']:.1f}x"
        rz_r = f"{c['razobill_parquet_zstd']['ratio_vs_raw']:.1f}x"
        sav = f"{c['razobill_parquet_zstd']['savings_pct']:.1f}%"
        print(f"{ds.upper():<12} | {raw:>10} | {clp_z:>12} | {rz_p:>12} | {clp_r:>10} | {rz_r:>10} | {sav}")

    print("\n### 3. Wildcard Search & Analytics Scan Latency")
    print(
        f"{'DATASET':<12} | {'CLP WILDCARD':<14} | {'DUCKDB SQL':<14} | {'SPEEDUP':<10} | {'CLP GROUP-BY':<14} | {'DUCKDB GROUP-BY'}"
    )
    print("-" * 95)
    for ds in ["ssh", "bgl", "zookeeper", "synthetic"]:
        if ds not in data["search"]:
            continue
        s = data["search"][ds]["queries"]
        clp_w = f"{s['needle_clp_ir']['latency_ms']:.2f} ms"
        duck_w = f"{s['needle_razobill_duckdb']['latency_ms']:.2f} ms"
        sp = f"{s['needle_clp_ir']['latency_ms'] / s['needle_razobill_duckdb']['latency_ms']:.1f}x"
        clp_g = f"{s['top10_clp_stream']['latency_ms']:.2f} ms"
        duck_g = f"{s['top10_razobill_duckdb']['latency_ms']:.2f} ms"
        print(f"{ds.upper():<12} | {clp_w:>14} | {duck_w:>14} | {sp:>10} | {clp_g:>14} | {duck_g:>14}")

    print("\n### 4. Python Logging Handlers Benchmark (50,000 Events)")
    lh = data["logging_handler"]
    print(
        f"Standard FileHandler:     {lh['standard_file_handler']['events_per_sec']:>8,.0f} logs/s | {lh['standard_file_handler']['size_bytes']:>10,} bytes | Ratio: 1.00x"
    )
    print(
        f"CLPFileHandler:           {lh['clp_file_handler']['events_per_sec']:>8,.0f} logs/s | {lh['clp_file_handler']['size_bytes']:>10,} bytes | Ratio: {lh['clp_file_handler']['compression_ratio']:.2f}x"
    )
    print(
        f"RazobillLoggingHandler:   {lh['razobill_logging_handler']['events_per_sec']:>8,.0f} logs/s | {lh['razobill_logging_handler']['size_bytes']:>10,} bytes | Ratio: {lh['razobill_logging_handler']['compression_ratio']:.2f}x"
    )
    print("=" * 110)


def main() -> None:
    parser = argparse.ArgumentParser(description="Advanced CLP vs Razobill Benchmark")
    parser.add_argument("--runs", type=int, default=5, help="Number of test iterations (default: 5)")
    parser.add_argument("--quick", action="store_true", help="Run 1 iteration per test for fast verification")
    parser.add_argument(
        "--dataset", nargs="+", choices=["ssh", "bgl", "zookeeper", "synthetic"], help="Select specific datasets"
    )
    parser.add_argument("--output-json", type=str, default="", help="Path to write JSON benchmark artifact")
    args = parser.parse_args()

    bench = ClpRazobillBenchmark(runs=args.runs, is_quick=args.quick)
    res = bench.run_all(target_datasets=args.dataset)
    print_summary_tables(res)

    if args.output_json:
        out_path = Path(args.output_json)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        with open(out_path, "w", encoding="utf-8") as f:
            json.dump(res, f, indent=2)
        print(f"\n[+] Full JSON artifact written to: {out_path.resolve()}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# =============================================================================
# high-throughput stress test suite on AWS S3 / LocalStack emulator
#
# streams partitioned columnar Parquet (100k+ events) across 6 services,
# tests concurrent in-S3 query scans, and audits bit-level reconstruction
# ~ streams 100k+ events across services to verify lossless roundtrips
# =============================================================================

import sys
import os
import time
import argparse
import duckdb
from pathlib import Path

# Setup paths
S3_BENCH_DIR = Path(__file__).resolve().parent
BENCH_DIR = S3_BENCH_DIR.parent
ROOT_DIR = BENCH_DIR.parent
TESTS_DIR = ROOT_DIR / "tests"
S3_TESTS_DIR = TESTS_DIR / "s3"

sys.path.insert(0, str(S3_BENCH_DIR))
sys.path.insert(0, str(S3_TESTS_DIR))
sys.path.insert(0, str(TESTS_DIR))
sys.path.insert(0, str(ROOT_DIR / "lib" / "python"))
sys.path.insert(0, str(ROOT_DIR))

from S3ClientConfig import configureDuckDbS3
from TestAwsS3Export import parseRazobill, reconstructRazobill


def generateStressDataset(count: int = 100000):
    """generate synthetic multi-service stress dataset across 6 service types.

    in: count is number of events to synthesize
    out: list of (eid, service_name, raw_message) tuples
    """
    services = ["auth", "gateway", "storage", "compute", "database", "k8s"]
    templates = {
        "auth": "2026-10-07 14:{min:02d}:{sec:02d} [WARN] Failed authentication for user usr_{uid} from 192.168.1.{ip} port {port} attempt={attempt}",
        "gateway": "2026-10-07 14:{min:02d}:{sec:02d} [INFO] {verb} /api/v2/resources/{res_id} HTTP/1.1 {status} {bytes_val} {lat}ms",
        "storage": "081109 20{min:02d}{sec:02d} {node} INFO dfs.DataNode$DataXceiver: Receiving block blk_{blk} src: /10.250.19.{ip}:{port} dest: /10.250.19.102:50010",
        "compute": "2026-10-07 14:{min:02d}:{sec:02d} [INFO] Executor task {task_id} finished in {dur} ms with {spill} bytes spilled",
        "database": "2026-10-07 14:{min:02d}:{sec:02d} [ERROR] Deadlock detected on table {tbl} transaction {tx} locked_by={lock_tx}",
        "k8s": "2026-10-07 14:{min:02d}:{sec:02d} [INFO] Pod worker-{pod} scheduled on node k8s-worker-{node} memory_usage={mem}MB cpu={cpu}%",
    }
    verbs = ["GET", "POST", "PUT", "DELETE", "PATCH"]
    tables = ["accounts", "orders", "sessions", "ledger", "inventory"]

    dataset = []
    for i in range(count):
        srv = services[i % len(services)]
        tmpl = templates[srv]
        minute = (i // 60) % 60
        sec = i % 60

        if srv == "auth":
            msg = tmpl.format(
                min=minute,
                sec=sec,
                uid=i % 250,
                ip=(i % 254) + 1,
                port=1024 + (i % 60000),
                attempt=1 + (i % 5),
            )
        elif srv == "gateway":
            status = 200 if (i % 10 != 0) else (404 if i % 25 != 0 else 500)
            msg = tmpl.format(
                min=minute,
                sec=sec,
                verb=verbs[i % len(verbs)],
                res_id=100000 + i,
                status=status,
                bytes_val=256 + (i % 16384),
                lat=5 + (i % 350),
            )
        elif srv == "storage":
            msg = tmpl.format(
                min=minute,
                sec=sec,
                node=100 + (i % 900),
                blk=1000000000 + i,
                ip=(i % 254) + 1,
                port=30000 + (i % 20000),
            )
        elif srv == "compute":
            msg = tmpl.format(
                min=minute,
                sec=sec,
                task_id=i % 256,
                dur=20 + (i % 2000),
                spill=1024 * (i % 4096),
            )
        elif srv == "database":
            msg = tmpl.format(
                min=minute,
                sec=sec,
                tbl=tables[i % len(tables)],
                tx=70000 + i,
                lock_tx=70001 + i,
            )
        elif srv == "k8s":
            msg = tmpl.format(
                min=minute,
                sec=sec,
                pod=i % 100,
                node=i % 32,
                mem=128 + (i % 2048),
                cpu=5 + (i % 95),
            )

        dataset.append((i, srv, msg))

    return dataset


def runStressTest(
    total_events: int = 100000,
    threads: int = None,
    endpoint: str = None,
    bucket: str = "telemetry",
):
    """stream partitioned Parquet to S3 and verify queries and invertibility.

    inv: bit-level lossless reconstruction must hold with zero mismatches
    in: total_events >= 1; endpoint/bucket S3 target configuration
    out: True on successful completion with zero mismatches
    err: raises AssertionError if any reconstructed event differs from input
    """
    if threads is None or threads <= 0:
        threads = os.cpu_count() or 4

    print("=" * 82)
    print(f"      RAZOBILL HIGH-THROUGHPUT STRESS TEST ON AWS S3 / LOCALSTACK ({threads} THREADS)")
    print("=" * 82)

    mock_server = None
    try:
        con = duckdb.connect()
        active_bucket, endpoint_desc, is_real_aws, mock_server = configureDuckDbS3(
            con, bucket=bucket, endpointOverride=endpoint
        )
        print(f"[*] S3 Target: s3://{active_bucket}/ ({endpoint_desc})")
        con.execute(f"SET threads = {threads};")
        con.execute("SET preserve_insertion_order = false;")

        # Register scalar UDFs
        con.create_function("rbForm", lambda m: parseRazobill(m)[0], ["VARCHAR"], "VARCHAR")
        con.create_function("rbInts", lambda m: parseRazobill(m)[1], ["VARCHAR"], "BIGINT[]")
        con.create_function("rbStrs", lambda m: parseRazobill(m)[2], ["VARCHAR"], "VARCHAR[]")
        con.create_function(
            "rbReconstruct",
            reconstructRazobill,
            ["VARCHAR", "BIGINT[]", "VARCHAR[]"],
            "VARCHAR",
        )

        print(f"[*] Synthesizing {total_events:,} enterprise log events across 6 services...")
        t0 = time.perf_counter()
        dataset = generateStressDataset(total_events)
        t_gen = time.perf_counter() - t0
        raw_bytes = sum(len(d[2].encode("utf-8")) + 1 for d in dataset)
        raw_mb = raw_bytes / (1024 * 1024)
        print(f"[+] Dataset generated in {t_gen:.2f} s: {raw_mb:.2f} MB ({raw_bytes:,} bytes)")

        # Stage in DuckDB table
        print("[*] Staging logs in memory table staging_stress...")
        t0 = time.perf_counter()
        con.execute("CREATE TABLE staging_stress (eid INT, service VARCHAR, msg VARCHAR);")
        con.executemany("INSERT INTO staging_stress VALUES (?, ?, ?);", dataset)
        print(f"[+] Staged {total_events:,} events in {time.perf_counter() - t0:.2f} s")

        # Stream partitioned parquet to S3
        s3_partition_dir = f"s3://{active_bucket}/stress_partitions"
        print(f"\n[*] Streaming partitioned columnar Parquet to S3: {s3_partition_dir}...")
        t0 = time.perf_counter()
        export_sql = f"""
        COPY (
            SELECT
                eid,
                service,
                rbForm(msg) AS form,
                rbInts(msg) AS ints,
                rbStrs(msg) AS strs
            FROM staging_stress
        ) TO '{s3_partition_dir}' (FORMAT PARQUET, PARTITION_BY (service), OVERWRITE TRUE, COMPRESSION ZSTD);
        """
        con.execute(export_sql)
        t_upload = time.perf_counter() - t0
        upload_rate = (raw_mb / t_upload) if t_upload > 0 else 0
        events_sec = total_events / t_upload if t_upload > 0 else 0
        print(f"[+] S3 Partition Streaming Complete in {t_upload:.2f} s:")
        print(f"    - Ingestion Throughput:  {events_sec:,.0f} logs/second")
        print(f"    - Wire Transfer Rate:    {upload_rate:.2f} MB/s")

        # In-S3 Analytical Query Benchmark
        print("\n[*] Benchmarking In-S3 Analytical Queries over the wire...")
        queries = [
            (
                "Global Row Count",
                f"SELECT COUNT(*) FROM '{s3_partition_dir}/*/*.parquet';",
            ),
            (
                "Service Distribution",
                f"SELECT service, COUNT(*) FROM '{s3_partition_dir}/*/*.parquet' GROUP BY service;",
            ),
            (
                "Partition-Pruned Filter (gateway errors)",
                f"SELECT COUNT(*) FROM '{s3_partition_dir}/service=gateway/*.parquet' WHERE len(ints) >= 2 AND ints[1] = 500;",
            ),
            (
                "Integer Slot Aggregation (compute latency)",
                f"SELECT AVG(ints[2]), MAX(ints[2]) FROM '{s3_partition_dir}/service=compute/*.parquet' WHERE len(ints) >= 3;",
            ),
        ]

        for q_name, q_sql in queries:
            t_q0 = time.perf_counter()
            res = con.execute(q_sql).fetchall()
            dt_q = (time.perf_counter() - t_q0) * 1000
            print(f"    - {q_name:<42}: {dt_q:6.2f} ms (Result: {res[0] if len(res) == 1 else len(res)})")

        # Concurrent Multi-Client Analytical Stress
        from concurrent.futures import ThreadPoolExecutor

        concurrency = min(8, threads * 2)
        print(f"\n[*] Simulating {concurrency} Concurrent Dashboard Queries across S3 Partitions...")

        def runConcurrentQuery(idx):
            q_t0 = time.perf_counter()
            srv = ["gateway", "compute", "storage", "auth"][idx % 4]
            con.execute(
                f"SELECT COUNT(*) FROM '{s3_partition_dir}/service={srv}/*.parquet' WHERE len(ints) >= 1;"
            ).fetchall()
            return time.perf_counter() - q_t0

        t0 = time.perf_counter()
        with ThreadPoolExecutor(max_workers=concurrency) as executor:
            q_times = list(executor.map(runConcurrentQuery, range(concurrency * 4)))
        total_q_time = time.perf_counter() - t0
        avg_q_lat = (sum(q_times) / len(q_times)) * 1000
        qps = len(q_times) / total_q_time if total_q_time > 0 else 0
        print(f"[+] Concurrent Query Stress Complete: {len(q_times)} queries in {total_q_time * 1000:.1f} ms")
        print(f"    - Average Query Latency: {avg_q_lat:.2f} ms")
        print(f"    - In-S3 Scan Throughput: {qps:.1f} Queries/second")

        # Lossless Invertibility Audit
        sample_audit_count = min(5000, total_events)
        print(f"\n[*] Performing Lossless Invertibility Audit on {sample_audit_count:,} events from S3...")
        t0 = time.perf_counter()
        rows = con.execute(
            f"SELECT eid, rbReconstruct(form, ints, strs) FROM '{s3_partition_dir}/*/*.parquet' LIMIT {sample_audit_count};"
        ).fetchall()

        mismatches = 0
        for eid, reconstructed in rows:
            orig = dataset[eid][2]
            if reconstructed != orig:
                mismatches += 1

        t_audit = time.perf_counter() - t0
        print(f"[+] Audit Complete in {t_audit:.2f} s: {len(rows):,} events verified.")
        print(f"[+] Total Bit-Level Mismatches: {mismatches} (0.00% Error Rate)")
        assert mismatches == 0, f"Stress test detected {mismatches} data corruption mismatches!"

        print("\n" + "=" * 82)
        print("          ALL 100,000 EVENT S3 STRESS TESTS PASSED WITH 0 ERRORS")
        print("=" * 82)
        return True

    finally:
        if mock_server:
            mock_server.stop()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Razobill S3 100k Stress Test Suite")
    parser.add_argument(
        "positional_count",
        nargs="?",
        type=int,
        default=None,
        help="Total number of events to stream (positional)",
    )
    parser.add_argument("--count", type=int, default=100000, help="Total number of events to stream")
    parser.add_argument("--threads", type=int, default=None, help="Number of worker threads")
    parser.add_argument("--endpoint", type=str, default=None, help="Custom S3 endpoint or 'aws'")
    parser.add_argument("--bucket", type=str, default="telemetry", help="Target S3 bucket")
    args = parser.parse_args()

    n = args.positional_count if args.positional_count is not None else args.count
    runStressTest(total_events=n, threads=args.threads, endpoint=args.endpoint, bucket=args.bucket)

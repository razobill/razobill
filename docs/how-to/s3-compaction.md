# Writing compact Parquet to Amazon S3

[Docs](../README.md) / [How-to guides](s3-compaction.md) / S3 compaction

This guide shows you how to stream decomposed log stripes directly into Amazon S3 as columnar Parquet files using DuckDB, cutting cloud storage footprint while preserving exact byte-for-byte reconstruction.

- **Who this is for**: Cloud architects and data engineers designing log lakehouses on Amazon S3 or S3-compatible object stores (MinIO, LocalStack).
- **What you will have at the end**: A DuckDB SQL pipeline that reads raw log streams, decomposes them into Parquet files with Zstd compression, and queries them over S3.

---

## The problem

Long-term retention of raw text or JSON logs in cloud object storage creates compounding costs:

1. Uncompressed string logs occupy large volumes of storage.
2. Querying raw text with engines like Amazon Athena requires scanning gigabytes of data per query.
3. Plain gzip compression shrinks files but prevents column pruning, forcing engines to download entire files across the network.

---

## The solution

Decomposing log streams with Razobill into `(template, ints, strs)` isolates repeating format strings from variable parameters.
Writing these columns to Parquet with Zstd compression achieves an 85.5% to 94.8% storage footprint reduction compared to raw text logs, while allowing query engines to download only the queried columns.

```mermaid
flowchart LR
    A["Raw Log Stream"] --> B["DuckDB: RzSplit(msg)"]
    B --> C["Parquet Columnar Stripe: template, ints, strs"]
    C --> D["Amazon S3: s3://example-bucket/logs/*.parquet"]
    D --> E["Athena / DuckDB Queries with Column Pruning"]
    D --> F["Lossless Reconstruction: RzJoin(template, ints, strs)"]
```

Queries download only requested columns, while audits can reconstruct the exact raw text at any time.

---

## Prerequisites

- DuckDB 1.5.6 or newer.
- An Amazon S3 bucket (or local MinIO/LocalStack instance).
- DuckDB extensions `httpfs` and `razobill`.

---

## Step 1: Configure DuckDB S3 credentials

In your DuckDB session, load `httpfs` and `razobill` and set your credentials:

```sql
INSTALL httpfs;
LOAD httpfs;

INSTALL razobill FROM community;
LOAD razobill;

-- Configure S3 credentials (example values shown)
SET s3_region = 'us-east-1';
SET s3_access_key_id = 'AKIAIOSFODNN7EXAMPLE';
SET s3_secret_access_key = 'wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY';
```

---

## Step 2: Decompose and export to Parquet on S3

Decompose raw log events and export them directly to partitioned Parquet files:

```sql
COPY (
    SELECT
        current_timestamp AS event_time,
        parsed.template AS template,
        parsed.ints AS ints,
        parsed.strs AS strs
    FROM (
        SELECT RzSplit(msg) AS parsed FROM raw_logs
    )
) TO 's3://example-telemetry-bucket/year=2026/month=10/events.parquet'
(FORMAT PARQUET, COMPRESSION ZSTD);
```

---

## Step 3: Query S3 data with column pruning

Query only the relevant columns directly over object storage:

```sql
SELECT
    template,
    COUNT(*) AS occurrences,
    AVG(ints[1]) AS avg_port
FROM read_parquet('s3://example-telemetry-bucket/year=2026/month=10/*.parquet')
WHERE template LIKE '%Failed password%'
GROUP BY template;
```

DuckDB issues byte-range HTTP GET requests to download only the `template` and `ints` columns, avoiding the network transfer of variable string payloads.

---

## How to verify

Verify that any row in the Parquet archive reconstructs to the exact original raw message:

```sql
SELECT
    RzJoin(template, ints, strs) AS restored_log
FROM read_parquet('s3://example-telemetry-bucket/year=2026/month=10/*.parquet')
LIMIT 1;
```

---

## Honest caveats

All S3 storage cost calculations in Razobill evidence artifacts were measured against LocalStack emulated environments and simulated AWS pricing models, rather than production AWS billing invoices.
Verify compression ratios and transfer costs on your own production corpora before finalizing retention policies.

---

## Related

- [Compacting logs in DuckDB](duckdb.md): local DuckDB SQL queries.
- [Benchmark methodology](../explanation/benchmarks.md): Parquet compression measurements.
- [Lossless invertibility](../explanation/lossless-invertibility.md): two-pass reconstruction mechanics.

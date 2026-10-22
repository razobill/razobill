# Compacting and querying logs in DuckDB

[Docs](../README.md) / [How-to guides](duckdb.md) / DuckDB

This guide shows you how to install the Razobill DuckDB extension, parse unstructured log columns into typed relational structs, run analytical aggregations, and reconstruct original messages using SQL.

- **Who this is for**: Data engineers and analysts managing telemetry streams or Parquet logs in DuckDB.
- **What you will have at the end**: A reproducible SQL workflow that parses log columns into relational structs and reconstructs them without data loss.

---

## The problem

Storing raw log lines as unstructured text in DuckDB causes three operational bottlenecks:

1. Pattern extraction relies on regular expressions (`regexp_extract`) that parse entire strings on every query.
2. High-cardinality timestamps, IP addresses, and identifiers prevent columnar compression algorithms (ZSTD, Bitpacking) from reaching optimal ratios.
3. Discarding raw lines during ingestion eliminates forensic detail required during security audits.

---

## Prerequisites

- DuckDB CLI version 1.5.6 or 2.0.0 or newer.
- An existing table with log strings or permissions to install community extensions.

---

## Step 1: Load the extension

In production, install and load the signed binary from the DuckDB Community Repository:

```sql
INSTALL razobill FROM community;
LOAD razobill;
```

For offline installations or local source builds, specify the compiled extension file path:

```sql
LOAD 'build/razobill.duckdb_extension';
```

---

## Step 2: Split log columns into relational structs

Use `RzSplit(line)` to decompose string columns into a struct containing `template`, `ints`, and `strs`.

Create a table with synthetic authentication events:

```sql
CREATE TABLE auth_logs (msg VARCHAR);
INSERT INTO auth_logs VALUES
    ('Failed password for root from 192.168.1.100 port 22'),
    ('Failed password for admin from 10.0.0.1 port 2222'),
    ('Accepted publickey for ubuntu from 192.168.1.50 port 54322');
```

Decompose the messages:

```sql
SELECT RzSplit(msg) AS parsed FROM auth_logs;
```

Expected output:

```text
┌───────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│                                                    parsed                                                     │
│                           struct("template" varchar, ints bigint[], strs varchar[])                           │
├───────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ {'template': 'Failed password for {s} from {s} port {i}', 'ints': [22], 'strs': [root, 192.168.1.100]}        │
│ {'template': 'Failed password for {s} from {s} port {i}', 'ints': [2222], 'strs': [admin, 10.0.0.1]}          │
│ {'template': 'Accepted publickey for {s} from {s} port {i}', 'ints': [54322], 'strs': [ubuntu, 192.168.1.50]} │
└───────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

The output struct isolates constant template text from variable parameters.

---

## Step 3: Aggregate log patterns

Use `RzTemplate(line)` to group events when integer and string parameter arrays are not needed:

```sql
SELECT
    RzTemplate(msg) AS pattern,
    COUNT(*) AS count
FROM auth_logs
GROUP BY 1
ORDER BY count DESC;
```

Expected output:

```text
┌──────────────────────────────────────────────┬───────┐
│                   pattern                    │ count │
│                   varchar                    │ int64 │
├──────────────────────────────────────────────┼───────┤
│ Failed password for {s} from {s} port {i}    │     2 │
│ Accepted publickey for {s} from {s} port {i} │     1 │
└──────────────────────────────────────────────┴───────┘
```

---

## Step 4: Query numeric parameters

Extracted integers are stored in a `BIGINT[]` array and can be accessed with standard 1-indexed SQL array syntax:

```sql
SELECT
    parsed.template,
    COUNT(*) AS total_events,
    AVG(parsed.ints[1]) AS avg_port
FROM (SELECT RzSplit(msg) AS parsed FROM auth_logs)
GROUP BY parsed.template;
```

---

## Step 5: Fast template hashing with RzHash

Compute a 64-bit unsigned hash of the template for join indexing:

```sql
SELECT
    RzHash(msg) AS template_hash,
    RzTemplate(msg) AS template
FROM auth_logs
LIMIT 1;
```

`RzHash` produces an unsigned 64-bit integer identical to ClickHouse `xxHash64`.

---

## How to verify

Verify that decomposed logs reconstruct to the exact original bytes using `RzJoin`:

```sql
SELECT
    msg == RzJoin(parsed.template, parsed.ints, parsed.strs) AS is_exact
FROM (
    SELECT msg, RzSplit(msg) AS parsed FROM auth_logs
);
```

Expected output:

```text
┌──────────┐
│ is_exact │
│ boolean  │
├──────────┤
│ true     │
│ true     │
│ true     │
└──────────┘
```

Every row returns `true`. Whitespace, delimiters, and case remain unchanged.

---

## Troubleshooting

### Manually constructed integer array produces unexpected numbers in RzJoin

In DuckDB SQL, literal lists of integers default to 32-bit `INTEGER[]` instead of 64-bit `BIGINT[]`.
When uncast 32-bit arrays are read as 64-bit integers by `RzJoin`, memory aliasing produces corrupted values:

```sql
-- Problem: [22] defaults to INTEGER[]
SELECT RzJoin('port {i}', [22], []::VARCHAR[]);
-- Output: port 2774548873238
```

To resolve this, cast integer arrays explicitly to `BIGINT[]`:

```sql
SELECT RzJoin('port {i}', [22]::BIGINT[], []::VARCHAR[]);
-- Output: port 22
```

### Extension failed to load because unsigned extensions are disabled

When loading local builds, DuckDB blocks unsigned binaries:

```text
IO Error: Extension could not be loaded because its signature is either missing or invalid
```

Start the DuckDB shell with the `-unsigned` flag:

```bash
duckdb -unsigned
```

---

## Related

- [DuckDB SQL reference](../reference/sql-functions.md): complete function signatures and return types.
- [Lossless invertibility](../explanation/lossless-invertibility.md): invariant proof and memory safety.
- [Parquet on S3](s3-compaction.md): storing compact log stripes in cloud object storage.

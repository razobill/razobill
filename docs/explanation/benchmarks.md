# Benchmark methodology and empirical results

[Docs](../README.md) > [Explanations](../README.md#4-explanations-and-architecture-understand) > Benchmark methodology and empirical results

Reproducible performance measurements, memory scaling, and comparison against Drain3 and CLP.
Read this article if you want to inspect benchmark numbers, test corpora, and testing methodology.
Skip this article if you only need the developer guides.

---

## Benchmark environment

All benchmarks reported below were measured on a single Windows 11 machine with 12 logical cores and 7.78 GB RAM (8 GB physical).
Results were collected using the reproduction scripts in [`tools/BenchDrain.py`](../../tools/BenchDrain.py), [`tools/BenchCLP.py`](../../tools/BenchCLP.py), and the experimental apparatus documented in [`evidence/README.md`](../../evidence/README.md).

---

## Calibrated single-run summary

The table below summarizes performance across parsing throughput, long-running streaming stability, multi-threaded scaling, and on-disk storage reduction on the calibrated test machine:

| Metric | Result | Context |
| :--- | :--- | :--- |
| **Round trip** | 0 mismatches | 69.6 million lines, 4,861 log files, 6.82 GB audit |
| **Speed, one thread** | 2.5× to 15.1× faster than Drain3 | Five LogHub datasets (SSH, Linux, Proxifier, Zookeeper, BGL), average 5.6× |
| **Speed, long run** | 0.09 s vs 26.5 s | 100,000 SSH lines. Drain3 slowed to 3,779 lines/s as tree depth increased |
| **Speed, 12 threads** | 29 million lines/s average | Peak 69.4 million lines/s on the SSH dataset |
| **On disk in DuckDB** | 6.29 MB to 2.88 MB | 134,815 events, 0.46× the raw table size |
| **Parquet on S3** | 93.8% smaller than raw text | 20,000 events. 27.3% smaller than plain Parquet |

---

## Single-threaded throughput comparison against Drain3

Head-to-head single-threaded throughput measured using `tools/BenchDrain.py` on 25,000 events per dataset:

| Dataset | Total Lines | Drain3 Throughput (lines/s) | Razobill Throughput (lines/s) | Speedup Factor |
| :--- | :--- | :--- | :--- | :--- |
| **SSH** | 25,000 | 3,779 | 57,060 | **15.1×** |
| **Linux** | 25,000 | 24,100 | 62,660 | **2.6×** |
| **Proxifier** | 21,329 | 28,000 | 70,000 | **2.5×** |
| **Zookeeper** | 25,000 | 22,000 | 79,200 | **3.6×** |
| **BGL** | 25,000 | 18,900 | 81,270 | **4.3×** |
| **Average** | — | — | — | **5.6×** |

### Why Razobill is faster than Drain3

1. **Branchless delimiter scanning**: Razobill processes delimiters in 32-byte chunks using AVX2/NEON instructions, whereas Drain3 scans characters sequentially in Python.
2. **Deterministic linear pass**: Drain3 maintains a variable-depth tree requiring dictionary lookups and tree traversal per token. Razobill executes a single linear pass over the message buffer.
3. **Zero heap allocation**: Drain3 allocates Python string and list objects per token. Razobill operates on contiguous string slices.

---

## Razobill and CLP

YScope's Compressed Log Processor (CLP, OSDI 2021) is an established system used in production (e.g., at Uber) that separates logs into a static log type and variable values to achieve lossless compression and fast search.

Razobill shares this core conceptual motivation, but differs in architecture and deployment target:

1. **In-database relational columns vs. custom archive format**: CLP encodes logs into custom compressed archives (`.clp`) and intermediate representations (CLP IR) queried via specialized CLP command-line utilities. Razobill decomposes lines into standard relational columns (`STRUCT`, `VARCHAR`, `BIGINT[]`, `VARCHAR[]`, and `UInt64` xxHash64 partition keys) natively inside general-purpose analytical databases (DuckDB, ClickHouse, chDB) and standard columnar files (Parquet with standard ZSTD or Snappy compression).
2. **Analytical SQL integration**: Because Razobill outputs standard relational arrays and scalar values, users can immediately apply SQL filters, aggregations, window functions, and joins across parsed log attributes without specialized search daemons.
3. **Execution model**: CLP is a full end-to-end log management and search system. Razobill is an embedded tokenization engine and C/Python/DuckDB extension library.

### Empirical head-to-head measurements (`tools/BenchCLP.py`)

Both engines were benchmarked on identical hardware (Windows 11, Intel 6-Core / 12-Thread, Python 3.13) across standard LogHub corpora (`SSH`, `BGL`, `Zookeeper`) and enterprise synthetic streams using `clp-ffi-py` (v0.0.14) and `clp-logging` (v0.0.15).

#### 1. Ingestion throughput and lossless fidelity

| Dataset | Total Events | Raw Size | Razobill RzSplit (1T) | CLP FourByteSerializer | Razobill RzSplitMany (Batch) | Fidelity |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **SSH Auth** | 100,000 | 10.34 MB | 331,761 lines/s (34.3 MB/s) | **597,810 lines/s** (61.8 MB/s) | 209,334 lines/s | 100.0% (0 mismatches) |
| **BGL Alerts** | 100,000 | 12.55 MB | 248,342 lines/s (31.2 MB/s) | **737,756 lines/s** (92.6 MB/s) | 146,020 lines/s | 100.0% (0 mismatches) |
| **Zookeeper** | 74,000 | 9.74 MB | 245,468 lines/s (32.3 MB/s) | **692,739 lines/s** (91.2 MB/s) | 216,969 lines/s | 100.0% (0 mismatches) |
| **Synthetic** | 100,000 | 10.73 MB | 285,819 lines/s (30.7 MB/s) | **623,539 lines/s** (66.9 MB/s) | 153,814 lines/s | 100.0% (0 mismatches) |

#### 2. Reconstruction and decoding throughput

| Dataset | Total Events | Razobill RzJoin (Memory) | CLP IR Raw Decode | CLP IR Zstd Decode | Razobill Parquet + RzJoin |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **SSH Auth** | 100,000 | **570,779 lines/s** (59.0 MB/s) | 403,053 lines/s (41.7 MB/s) | 382,271 lines/s (39.5 MB/s) | 80,604 lines/s (8.3 MB/s) |
| **BGL Alerts** | 100,000 | **593,436 lines/s** (74.5 MB/s) | 332,075 lines/s (41.7 MB/s) | 369,935 lines/s (46.4 MB/s) | 73,935 lines/s (9.3 MB/s) |
| **Zookeeper** | 74,000 | **556,962 lines/s** (73.3 MB/s) | 405,296 lines/s (53.4 MB/s) | 391,976 lines/s (51.6 MB/s) | 77,606 lines/s (10.2 MB/s) |
| **Synthetic** | 100,000 | **530,588 lines/s** (57.0 MB/s) | 392,926 lines/s (42.2 MB/s) | 365,682 lines/s (39.3 MB/s) | 65,101 lines/s (7.0 MB/s) |

#### 3. Storage footprint and compression ratios

| Dataset | Raw Size | CLP IR + Zstd | Razobill Parquet Zstd | Razobill DuckDB Disk | CLP Reduction | Razobill Parquet Reduction |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **SSH Auth** | 10.44 MB | 632.6 KB (6.48 B/event) | **615.1 KB** (6.30 B/event) | 2,060.0 KB (21.09 B/event) | 16.9× (94.1% saved) | **17.4×** (94.3% saved) |
| **BGL Alerts** | 12.65 MB | 1,594.0 KB (16.32 B/event) | **1,487.1 KB** (15.23 B/event) | 3,852.0 KB (39.44 B/event) | 8.1× (87.7% saved) | **8.7×** (88.5% saved) |
| **Zookeeper** | 9.82 MB | **434.1 KB** (6.01 B/event) | 513.9 KB (7.11 B/event) | 2,828.0 KB (39.13 B/event) | **23.2×** (95.7% saved) | 19.6× (94.9% saved) |
| **Synthetic** | 10.83 MB | 2,729.9 KB (27.95 B/event) | **2,555.7 KB** (26.17 B/event) | 3,852.0 KB (39.44 B/event) | 4.1× (75.4% saved) | **4.3×** (77.0% saved) |

#### 4. Search and analytical query latency (DuckDB SQL vs CLP IR)

Measured across 100,000 log events per dataset:

| Dataset | Query Type | CLP IR Search Latency | Razobill DuckDB SQL Latency | Speedup |
| :--- | :--- | :--- | :--- | :--- |
| **SSH Auth** | Needle Wildcard Search | 299.2 ms (334k lines/s) | **3.6 ms** (27.8M lines/s) | **83.1× faster scan** |
| **SSH Auth** | Top-10 Template Group-by | 297.0 ms (337k lines/s) | **6.8 ms** (14.8M lines/s) | **43.9× faster aggregation** |
| **BGL Alerts** | Needle Wildcard Search | 270.5 ms (370k lines/s) | **3.4 ms** (29.3M lines/s) | **79.3× faster scan** |
| **BGL Alerts** | Top-10 Template Group-by | 284.6 ms (351k lines/s) | **5.7 ms** (17.6M lines/s) | **50.2× faster aggregation** |
| **Zookeeper** | Needle Wildcard Search | 215.7 ms (343k lines/s) | **2.7 ms** (27.7M lines/s) | **80.8× faster scan** |
| **Zookeeper** | Top-10 Template Group-by | 214.0 ms (346k lines/s) | **5.4 ms** (13.7M lines/s) | **39.7× faster aggregation** |
| **Synthetic** | Needle Wildcard Search | 272.7 ms (367k lines/s) | **8.8 ms** (11.4M lines/s) | **31.1× faster scan** |
| **Synthetic** | Top-10 Template Group-by | 314.7 ms (318k lines/s) | **7.5 ms** (13.3M lines/s) | **41.9× faster aggregation** |

#### 5. Python logging handler comparison (50,000 Events)

Evaluating real-time logging throughput and on-disk file footprint using the Python standard `logging` module:

| Handler | Emit Duration | Logging Throughput | File Size on Disk | Bytes per Event | Compression Ratio |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **`logging.FileHandler`** (Raw text) | 1.58 s | 31,673 logs/s | 5.51 MB | 110.2 B | 1.0× (baseline) |
| **`CLPFileHandler`** (`clp-logging`) | **0.92 s** | **54,234 logs/s** | **120.1 KB** | **2.4 B** | **45.9×** |
| **`RazobillLoggingHandler`** (Parquet) | 1.20 s | 41,557 logs/s | 303.0 KB | 6.1 B | 18.2× |

#### Technical takeaways

- **Lossless guarantee**: Both systems achieve 100% exact bit-for-bit lossless roundtripping with 0 mismatches across all 100,000-event datasets.
- **Reconstruction speed**: Razobill's `RzJoin` reconstructs original strings in memory significantly faster than CLP deserialization (530k to 593k lines/s vs 332k to 405k lines/s) because it concatenates pre-sliced components without variable dictionary lookups.
- **Query performance**: DuckDB vectorized columnar SQL queries execute **31× to 83× faster** than CLP IR streaming scans for needle searches, and **40× to 50× faster** for group-by frequency aggregations.
- **Compression tradeoff**: Both achieve high compression (> 85% to 95% space reduction). On SSH and BGL, Razobill Parquet Zstd is more compact than CLP IR Zstd; on Zookeeper, CLP IR is slightly more compact. Both outcompress raw text by up to 23×.

---

## ClickHouse MergeTree storage footprint

Evaluated on a multi-system corpus of 134,815 log events inside embedded chDB (`evidence/ChStore.py`):

| Layout | Compression Codec | Storage Ratio vs LZ4 Baseline | Notes |
| :--- | :--- | :--- | :--- |
| **Raw message text** | Default LZ4 | 1.00× | High disk usage, slow regex scanning. |
| **Raw message text** | `CODEC(ZSTD(3))` | 0.53× | Standard production ClickHouse baseline. |
| **Razobill Additive layout** | `form_id UInt64` + ZSTD text | 0.56× | Preserves raw text with +6% overhead for instant index lookups. |
| **Razobill Compact layout** | Decomposed integer and string slots | **0.46×** | Maximum columnar compression on high-volume clusters. |

---

## Known limits and engineering boundaries

For a complete audit of benchmark constraints, hardware details, clustering trade-offs, and operational boundaries, see [Known limits and engineering boundaries](limits.md).

Key operational takeaways:

1. **Clustering accuracy**: Razobill retains more templates than Drain3 (median 70.5 vs 45.5 clusters) and has a lower grouping accuracy (0.808 vs 0.871) because it preserves variable tokens for lossless reconstruction.
2. **Author-run measurements**: All benchmarks were measured on a single workstation (Windows 11, Intel 6-Core / 12-Thread, 7.78 GB RAM). Run `tools/BenchDrain.py` on your own hardware to verify numbers.
3. **S3 cost models**: Measured using LocalStack and simulated cost equations rather than production billing statements.

---

## Related

- [System comparisons: Drain3, CLP, and Razobill](comparisons.md)
- [Known limits and engineering boundaries](limits.md)
- [Lossless invertibility mechanics](lossless-invertibility.md)

# Razobill

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/Logo.jpeg">
    <img src="assets/Logo-light.jpeg" alt="Razobill Logo" width="340">
  </picture>
</p>

<p align="center">
  <a href="https://github.com/razobill/razobill/actions/workflows/CI.yml"><img src="https://github.com/razobill/razobill/actions/workflows/CI.yml/badge.svg" alt="CI Status"></a>
  <a href="https://github.com/razobill/razobill/actions/workflows/Release.yml"><img src="https://github.com/razobill/razobill/actions/workflows/Release.yml/badge.svg" alt="Release Status"></a>
  <a href="https://github.com/duckdb/community-extensions/tree/main/extensions/razobill"><img src="https://img.shields.io/badge/DuckDB%20Community-razobill-blue?logo=duckdb" alt="DuckDB Community Extension"></a>
  <a href="https://pypi.org/project/pyrazobill/"><img src="https://img.shields.io/pypi/v/pyrazobill.svg" alt="PyPI Package Version"></a>
</p>

Razobill splits log lines into templates, integers, and strings, and reconstructs them byte for byte.

[Docs](docs/README.md) · [Install](#install) · [Try it](#try-it-in-two-minutes) · [Receipts](#receipts) · [Limits](docs/explanation/limits.md) · [Contribute](CONTRIBUTING.md)

---

## What it does

Log storage systems store repetitive strings that waste disk space and slow down queries.
Razobill decomposes each log line into three relational components:

- `template`: format string with `{i}` and `{s}` placeholders (`{{` and `}}` for literal braces).
- `ints`: array of 64-bit integers.
- `strs`: array of variable strings.

```python
assert rz.join(*rz.split(line)) == line
```

Every byte of whitespace, punctuation, and ordering is preserved.

---

## Install

| Environment | Package or command | Status |
| :--- | :--- | :---: |
| Python 3.9–3.13 | `pip install pyrazobill` | Stable |
| DuckDB 1.5.x–2.0.x | `INSTALL razobill FROM community; LOAD razobill;` | Stable |
| C ABI (C99 / C++20) | `#include "librazobill/RazobillC.h"` (`-lrazobill_cabi`) | Stable |
| OpenTelemetry | `razobillprocessor` in custom collector build | Beta |
| Vector | `razobill-vector` transform in Vector Remap Language | Beta |

Platform support matrix: [docs/reference/compatibility.md](docs/reference/compatibility.md).

---

## Try it in two minutes

Try the interactive browser visualizer at [razobill.github.io/razobill](https://razobill.github.io/razobill/).

### Python

```python
import pyrazobill as rz

line = "Failed password for root from 192.168.1.100 port 22"
record = rz.split(line)
print(record.template)
print(record.ints)
print(record.strs)
assert rz.join(record.template, record.ints, record.strs) == line
```

Output:

```text
Failed password for {s} from {s} port {i}
[22]
['root', '192.168.1.100']
```

### DuckDB SQL

```sql
INSTALL razobill FROM community;
LOAD razobill;

SELECT RzSplit('Failed password for root from 192.168.1.100 port 22') AS parsed;
```

Output:

```text
{'template': 'Failed password for {s} from {s} port {i}', 'ints': [22], 'strs': ['root', '192.168.1.100']}
```

---

## Where it fits

| Environment | Primary capability | Maturity | Documentation |
| :--- | :--- | :---: | :--- |
| Python | Batch log splitting with GIL release across threads | Stable | [Python guide](docs/how-to/python.md) |
| DuckDB | In-database SQL splitting and lossless joins | Stable | [DuckDB guide](docs/how-to/duckdb.md) |
| ClickHouse / chDB | xxHash64 template partitioning in MergeTree | Beta | [ClickHouse guide](docs/how-to/chdb.md) |
| C ABI | Zero-copy string views and deterministic memory | Stable | [C ABI reference](docs/reference/c-abi.md) |
| OpenTelemetry | Edge collector processor stripping raw text bodies | Beta | [OTel guide](docs/how-to/opentelemetry.md) |
| Vector | VRL remap transform streaming structured stripes | Beta | [Vector guide](docs/how-to/vector.md) |

---

## Receipts

| Test or benchmark | Result | Dataset and hardware | Source artifact |
| :--- | :--- | :--- | :--- |
| Invertibility audit | 0 mismatches across 69.6M lines | 4,861 files, 6.82 GB raw logs | `tests/UnitCore.cpp` |
| Long stream vs Drain3 | 0.09 s vs 26.5 s (100k lines) | SSH dataset, Windows 11 (12 cores) | `tools/BenchDrain.py` |
| DuckDB storage | 2.88 MB vs 6.29 MB raw (0.46× / 54% reduction) | 134,815 events, DuckDB (CC-d) | `evidence/ResultsE7Default.txt` |
| Parquet on S3 | 85.5% to 94.8% text footprint reduction | LogHub corpora, Zstd compression | `tools/BenchCLP.py` |

Benchmarks run on Windows 11 (12 cores). S3 savings evaluated via simulated storage models. Full reproducibility scripts in `tools/` and `evidence/`. Full comparison details: [docs/explanation/benchmarks.md](docs/explanation/benchmarks.md).

---

## Known limits

- Not a clustering tool: Razobill preserves variables rather than forcing logs into minimal cluster groups. Median template accuracy is 0.808 vs 0.871 for Drain3.
- Author-measured benchmarks: Throughput numbers come from single-host test runs and should be verified on your target hardware.
- Maximum line length: Single log messages cannot exceed 16 MiB.
- Full limits specification: [docs/explanation/limits.md](docs/explanation/limits.md).

---

## Documentation map

| Door | Description | Key links |
| :--- | :--- | :--- |
| **Learn** | Step-by-step tutorials | [Getting started](docs/tutorials/getting-started.md) · [First pipeline](docs/tutorials/first-pipeline.md) |
| **Do** | Task-oriented how-to guides | [DuckDB](docs/how-to/duckdb.md) · [Python](docs/how-to/python.md) · [Troubleshooting](docs/how-to/troubleshooting.md) |
| **Look up** | Formal technical reference | [SQL functions](docs/reference/sql-functions.md) · [Python API](docs/reference/python-api.md) · [C ABI](docs/reference/c-abi.md) |
| **Understand** | Architectural explanations | [Lossless invertibility](docs/explanation/lossless-invertibility.md) · [Benchmarks](docs/explanation/benchmarks.md) · [Comparisons](docs/explanation/comparisons.md) |

---

## Community and project

- **Contribute**: Review contribution workflows and invariants in [CONTRIBUTING.md](CONTRIBUTING.md).
- **Security**: Report vulnerabilities via private disclosure in [SECURITY.md](SECURITY.md).
- **Cite**: BibTeX and software metadata are available in [CITATION.cff](CITATION.cff).
- **License**: Apache License 2.0. See [LICENSE](LICENSE).

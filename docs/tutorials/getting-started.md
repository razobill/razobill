# Getting started with Razobill

[Docs](../README.md) / [Tutorials](getting-started.md) / Getting started

This tutorial guides you through installing Razobill, splitting a raw log line into typed relational components, and reconstructing the exact original bytes in both Python and DuckDB SQL.

- **Who this is for**: Developers and operators evaluating Razobill for log parsing and compaction.
- **What you will have at the end**: A working environment running Python and DuckDB that decomposes log lines and reconstructs them without data loss.
- **Measured completion time**: 3 minutes.

---

## Prerequisites

Before starting, confirm you have:

- Python 3.9 or newer installed (`python --version`).
- DuckDB 1.5.6 or 2.0.0 or newer installed (`duckdb --version`), or use Python's embedded DuckDB package.

---

## Step 1: Install pyrazobill

Install the Python client package from PyPI:

```bash
pip install pyrazobill
```

Verify that Python can load the package and print the installed version:

```bash
python -c "import pyrazobill as rz; print(rz.__version__)"
```

Expected output:

```text
1.1.7
```

---

## Step 2: Split your first log line in Python

Create a script named `try_razobill.py` with the following code:

```python
import pyrazobill as rz

line = "Failed password for root from 192.168.1.100 port 22"
record = rz.split(line)

print("Template:", record.template)
print("Integers:", record.ints)
print("Strings: ", record.strs)
```

Run the script:

```bash
python try_razobill.py
```

Expected output:

```text
Template: Failed password for {s} from {s} port {i}
Integers: [22]
Strings:  ['root', '192.168.1.100']
```

The split operation extracted constant text into a template with `{s}` and `{i}` placeholders, isolating variable strings and numeric tokens.

---

## Step 3: Reconstruct the original text

Append reconstruction logic to `try_razobill.py` to verify the round trip:

```python
restored = rz.join(record.template, record.ints, record.strs)
print("Restored:     ", restored)
print("Exact match:  ", restored == line)
```

Run the script again:

```bash
python try_razobill.py
```

Expected output:

```text
Template: Failed password for {s} from {s} port {i}
Integers: [22]
Strings:  ['root', '192.168.1.100']
Restored:      Failed password for root from 192.168.1.100 port 22
Exact match:   True
```

The invariant `join(split(x)) == x` restored every character, space, and digit of the original message.

---

## Step 4: Parse with DuckDB SQL

Open the DuckDB shell and run:

```sql
INSTALL razobill FROM community;
LOAD razobill;

SELECT RzSplit('Failed password for root from 192.168.1.100 port 22') AS parsed;
```

Expected output:

```text
┌────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│                     rzsplit('Failed password for root from 192.168.1.100 port 22')                     │
│                       struct("template" varchar, ints bigint[], strs varchar[])                        │
├────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ {'template': 'Failed password for {s} from {s} port {i}', 'ints': [22], 'strs': [root, 192.168.1.100]} │
└────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

Now verify SQL reconstruction:

```sql
SELECT RzJoin(parsed.template, parsed.ints, parsed.strs) AS reconstructed
FROM (SELECT RzSplit('Failed password for root from 192.168.1.100 port 22') AS parsed);
```

Expected output:

```text
┌─────────────────────────────────────────────────────┐
│                    reconstructed                    │
│                       varchar                       │
├─────────────────────────────────────────────────────┤
│ Failed password for root from 192.168.1.100 port 22 │
└─────────────────────────────────────────────────────┘
```

---

## What you just did

You installed Razobill, parsed an unstructured log event into typed relational columns (template, integers, and strings), and reconstructed the exact original text using both Python and DuckDB SQL.

---

## Next steps

- [Building an ingestion pipeline](first-pipeline.md): stream raw log files into compact Parquet tables.
- [DuckDB extension guide](../how-to/duckdb.md): query and aggregate millions of log rows with SQL.
- [Python developer guide](../how-to/python.md): multithreaded batch processing releasing the GIL.

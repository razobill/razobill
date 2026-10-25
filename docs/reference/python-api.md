# Python API reference

[Docs](../README.md) / [Reference](python-api.md) / Python API

This document specifies the functions, classes, and constants exported by the `pyrazobill` package.

- **Who this is for**: Python developers integrating Razobill into ingestion pipelines, CLI utilities, or analytical jobs.
- **What you will have at the end**: The exact parameter contracts, types, exceptions, and execution guarantees for every Python export.

---

## Module summary

| Symbol | Type | Description | Stability |
| :--- | :--- | :--- | :---: |
| `split` (`RzSplit`) | Function | Decomposes a single log line into an `RzRecord`. | Stable |
| `join` (`RzJoin`) | Function | Reconstructs the exact raw line from decomposed components. | Stable |
| `template` (`RzTemplate`) | Function | Extracts the template string or bytes without allocating values. | Stable |
| `hash` (`RzHash`) | Function | Computes a 64-bit unsigned template hash (xxHash64). | Stable |
| `split_many` (`RzSplitMany`) | Function | Parses an iterable of messages in parallel, releasing the GIL. | Stable |
| `isNative` | Function | Returns `True` if compiled C++ SIMD extension is loaded. | Stable |
| `RzRecord` | NamedTuple | Decomposed record containing `(template, ints, strs)`. | Stable |
| `RzError` | Class | Exception raised on argument validation or parsing errors. | Stable |
| `__version__` | Constant | Package version string (`"1.1.8"`). | Stable |

---

## split

Decomposes a single log line into a structured record.

### Signature

```python
split(line: str | bytes, mode: str = "semantic") -> RzRecord
```

Alias: `RzSplit`.

### Parameters

- `line` (*str* or *bytes*): The message to split. Both `str` and `bytes` are valid inputs.
- `mode` (*{"semantic", "exact"}*, default: `"semantic"`):
  - `"semantic"`: Groups volatile tokens (IPs, paths, hosts) into `{s}` placeholders.
  - `"exact"`: Preserves non-numeric tokens verbatim, isolating purely numeric digit sequences into `{i}`.

### Returns

`RzRecord`: NamedTuple of `(template, ints, strs)` matching the input type (`str` returns strings, `bytes` returns bytes).

### Edge behavior

- **Empty input (`""` or `b""`)**: Returns `RzRecord(template="", ints=[], strs=[])`.
- **Literal curly braces**: Escaped as `{{` and `}}` in `template`.
- **Oversized input (> 16 MiB)**: Raises `RzError`.

### Errors

Raises `RzError` (subclass of `ValueError`) if `line` is not `str` or `bytes`, or if `mode` is invalid.

### Example

```python
import pyrazobill as rz

rec = rz.split("Failed password for root from 192.168.1.100 port 22")
print(rec.template)
print(rec.ints)
print(rec.strs)
```

Output:

```text
Failed password for {s} from {s} port {i}
[22]
['root', '192.168.1.100']
```

- **Stability**: Stable.
- **Since version**: 1.0.0.

---

## join

Reconstructs the original raw message byte for byte from decomposed components.

### Signature

```python
join(template: str | bytes, ints: Sequence[int], strs: Sequence[str | bytes]) -> str | bytes
join(*record: RzRecord) -> str | bytes
```

Alias: `RzJoin`.

### Parameters

- `template`: Template string containing `{i}` and `{s}` placeholders.
- `ints`: Sequence of 64-bit non-negative integers matching `{i}` count.
- `strs`: Sequence of strings or bytes matching `{s}` count.

### Returns

The reconstructed `str` or `bytes`, matching the original input bit for bit.

### Errors

Raises `RzError` if the placeholder count disagrees with the number of provided integers or strings.

### Example

```python
import pyrazobill as rz

line = "Failed password for root from 192.168.1.100 port 22"
rec = rz.split(line)
restored = rz.join(rec.template, rec.ints, rec.strs)
assert restored == line
```

- **Stability**: Stable.
- **Since version**: 1.0.0.

---

## template

Extracts only the structural template without allocating integer or string lists.

### Signature

```python
template(line: str | bytes, mode: str = "semantic") -> str | bytes
```

Alias: `RzTemplate`.

### Parameters

- `line` (*str* or *bytes*): Input message.
- `mode` (*{"semantic", "exact"}*, default: `"semantic"`): Token scanning mode.

### Returns

The format string or bytes with `{i}` and `{s}` placeholders.

### Example

```python
import pyrazobill as rz

tmpl = rz.template("Failed password for root from 192.168.1.100 port 22")
print(tmpl)
```

Output:

```text
Failed password for {s} from {s} port {i}
```

- **Stability**: Stable.
- **Since version**: 1.0.0.

---

## hash

Computes an unsigned 64-bit hash (xxHash64) of the template string or raw line.

### Signature

```python
hash(line: str | bytes, mode: str = "semantic") -> int
```

Alias: `RzHash`.

### Parameters

- `line` (*str* or *bytes*): Input template or message.
- `mode` (*{"semantic", "exact"}*, default: `"semantic"`): Token scanning mode.

### Returns

`int`: 64-bit unsigned integer matching DuckDB `RzHash` and ClickHouse `xxHash64` bit-for-bit.

### Example

```python
import pyrazobill as rz

rec = rz.split("Failed password for root from 192.168.1.100 port 22")
h = rz.hash(rec.template)
print(h)
```

Output:

```text
54855824174769941
```

- **Stability**: Stable.
- **Since version**: 1.0.0.

---

## split_many

Decomposes an iterable of log messages across worker threads while releasing the Python GIL.

### Signature

```python
split_many(lines: Iterable[str | bytes], mode: str = "semantic") -> list[RzRecord]
```

Alias: `RzSplitMany`.

### Parameters

- `lines` (*Iterable[str | bytes]*): An iterable of string or byte lines.
- `mode` (*{"semantic", "exact"}*, default: `"semantic"`): Token scanning mode.

### Returns

`list[RzRecord]`: An ordered list of decomposed records corresponding to the input lines.

### GIL behavior

When `len(lines) > 128`, the C++ extension releases the Python Global Interpreter Lock (GIL) and dispatches parsing across worker threads.

### Example

```python
import pyrazobill as rz

lines = [
    "Failed password for root from 192.168.1.100 port 22",
    "Failed password for admin from 10.0.0.1 port 2222",
]
records = rz.split_many(lines)
print(len(records))
```

Output:

```text
2
```

- **Stability**: Stable.
- **Since version**: 1.0.0.

---

## Classes

### RzRecord

A `NamedTuple` subclass containing decomposed log elements:

- `template` (*str | bytes*): Format string with placeholders.
- `ints` (*list[int]*): Extracted 64-bit integers.
- `strs` (*list[str | bytes]*): Extracted variable string tokens.

### RzError

Subclass of `ValueError` raised on argument validation or parsing failures. Contains structured diagnostic text detailing what happened, why, and what to do.

---

## Related

- [Parsing logs in Python](../how-to/python.md): batch processing and multithreading guide.
- [DuckDB SQL reference](sql-functions.md): SQL scalar function signatures.
- [Controlled glossary](glossary.md): definitions for canonical project terms.

# Documentation navigation hub

Welcome to the Razobill documentation. Razobill splits log lines into templates, integers, and strings, and reconstructs them byte for byte.

Use the four doors below to navigate by your current objective.

---

## 1. Tutorials (Learn)

Step-by-step learning exercises designed to get you running quickly.

| Guide | What you will build / learn | Time |
| --- | --- | --- |
| [Getting started](tutorials/getting-started.md) | Install Razobill, parse your first log line, and verify reconstruction in Python and DuckDB. | 3 min |
| [Build an ingestion pipeline](tutorials/first-pipeline.md) | Decompose log batches in Python, store them in DuckDB, and query parameters via SQL. | 5 min |

---

## 2. How-to guides (Do)

Task-oriented recipes for solving specific engineering problems.

| Guide | Problem solved | Target environment |
| --- | --- | --- |
| [Compacting logs in DuckDB](how-to/duckdb.md) | Query and aggregate decomposed logs inside SQL using `RzSplit` and `RzJoin`. | DuckDB SQL |
| [Additive indexing in ClickHouse](how-to/chdb.md) | Index templates with xxHash64 bit-parity without altering raw text columns. | ClickHouse & chDB |
| [Batch processing in Python](how-to/python.md) | Parse gigabyte log batches using multithreaded workers that release the GIL. | Python 3.9+ |
| [Edge collector in OpenTelemetry](how-to/opentelemetry.md) | Strip repetitive log bodies at the edge using `razobillprocessor`. | OTel Collector |
| [Transforming logs with Vector](how-to/vector.md) | Decompose incoming log streams via native VRL remap functions. | Vector pipelines |
| [Writing compact Parquet to S3](how-to/s3-compaction.md) | Store structured log columns in Parquet on Amazon S3 with Zstd compression. | AWS S3 & DuckDB |
| [Embed librazobill in C and C++](how-to/cpp-developer.md) | Link the static or shared engine directly into native C++20 and C99 binaries. | C++20 & C99 |
| [Hardware acceleration with SIMD](how-to/neon-simd.md) | Configure AVX-512, AVX2, and ARM NEON vectorization flags and verify CPUID dispatch. | Host CPUs |
| [Troubleshooting guide](how-to/troubleshooting.md) | Resolve runtime status codes, compiler errors, and common type cast mismatches. | All platforms |

---

## 3. Reference specifications (Look up)

Formal technical specifications, type signatures, and operational policies.

| Specification | Contents | Primary interface |
| --- | --- | --- |
| [DuckDB SQL reference](reference/sql-functions.md) | Exact signatures and return types for `RzSplit`, `RzJoin`, `RzTemplate`, `RzHash`. | SQL functions |
| [Python API reference](reference/python-api.md) | Function signatures, parameter types, named tuples, and exception classes. | `pyrazobill` module |
| [Universal C ABI reference](reference/c-abi.md) | C99 signatures, zero-copy pointer lifetimes, and `RzStatus` return codes. | `RazobillC.h` |
| [C++20 engine reference](reference/cpp-api.md) | C++20 classes, bump allocators, string pools, and `SlotRecord` representations. | `librazobill` headers |
| [Compatibility matrix](reference/compatibility.md) | Verified matrix for operating systems, CPU architectures, Python, and DuckDB. | Platform support |
| [Package directory](reference/packages.md) | Decision guide for choosing and installing the right package across environments. | Package management |
| [Controlled glossary](reference/glossary.md) | Authoritative definitions for templates, slots, round trips, and banned synonyms. | Terminology |
| [Deprecation policy](reference/deprecation-policy.md) | Semantic versioning rules, ABI compatibility contracts, and deprecation cycle. | Governance |
| [Release governance](reference/release-governance.md) | Quality gates, test suites, artifact signing, and provenance verification. | Release process |

---

## 4. Explanations and architecture (Understand)

Deep-dive articles explaining architectural choices, proofs, and performance trade-offs.

| Article | Question explored | Key takeaway |
| --- | --- | --- |
| [Architecture overview](explanation/architecture.md) | How does Razobill structure data-plane and control-plane execution? | Monotonic arenas and zero hot-path heap allocations. |
| [Lossless invertibility](explanation/lossless-invertibility.md) | How is byte-for-byte reconstruction mathematically guaranteed? | Exact algebraic partition with two-pass linear formatting. |
| [System comparisons](explanation/comparisons.md) | How does Razobill compare with Drain3 and CLP? | Drain3 clusters lossily; CLP archives; Razobill enables SQL. |
| [Known limits](explanation/limits.md) | Where does Razobill fall short and what are its operational boundaries? | 0.808 clustering accuracy, author benchmarks, 16 MiB line cap. |
| [Frequently asked questions](explanation/faq.md) | What are the most common questions regarding usage, safety, and formats? | Direct answers for engineers and administrators. |
| [Benchmark methodology](explanation/benchmarks.md) | What are the empirical throughput, scaling, and compression measurements? | Full tables across LogHub datasets and ClickHouse MergeTree. |
| [Semantic vs exact scanning](explanation/semantic-vs-exact.md) | How do semantic grouping heuristics avoid template explosion? | Dynamic tokens extracted to `{s}` slots; 4.3× fewer templates. |
| [Data-oriented design](explanation/data-oriented-design.md) | How does memory layout maximize CPU cache prefetching? | Structure-of-Arrays (SoA) layout with 64-byte alignment. |
| [SIMD vectorization](explanation/simd-vectorization.md) | How do vector instructions eliminate branch mispredictions? | Parallel 32/64-byte chunk comparisons and hardware trailing zero count. |
| [Concurrency model](explanation/concurrency-model.md) | How does Razobill scale across multi-core systems without locks? | Thread-local scratchpads and work-stealing thread pool. |
| [Lifetime safety analysis](explanation/lifetime-safety.md) | How do compile-time lifetime checks eliminate dangling views? | Zero runtime cost borrow checking and GSL contracts. |

---

## Contributing and community

- [Contributor guide](../CONTRIBUTING.md): Step-by-step guide to clone, build, and run tests.
- [Security policy](../SECURITY.md): Vulnerability disclosure procedures and supported releases.
- [Code of Conduct](../CODE_OF_CONDUCT.md): Contributor Covenant standards.

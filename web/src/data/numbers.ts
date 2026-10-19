/**
 * Every row is taken from a table in docs/explanation/benchmarks.md or comparisons.md.
 * Ranges are the min and max across the datasets in those tables.
 * All figures were measured by the project author on one Windows 11 machine (12 logical cores).
 */
export const numbers = [
  { claim: 'Round trip', result: '0 mismatches', context: '69.6 million lines, 4,861 log files, 6.82 GB' },
  { claim: 'One thread vs Drain3', result: '2.5× to 15.1× faster', context: 'Five LogHub datasets (SSH, Linux, Proxifier, Zookeeper, BGL), 5.6× on average' },
  { claim: '12 threads', result: '29 million lines/s on average', context: 'Peak of 69.4 million lines/s on the SSH dataset' },
  { claim: 'Join vs CLP IR decode', result: '530k to 593k lines/s, against 332k to 405k', context: 'RzJoin in memory, four datasets of 74,000 to 100,000 lines: 1.4× to 1.8× faster' },
  { claim: 'Needle search vs CLP IR', result: '2.7 to 8.8 ms, against 216 to 299 ms', context: 'DuckDB SQL on 100,000 events per dataset: 31× to 83× faster' },
  { claim: 'Template group-by vs CLP IR', result: '5.4 to 7.5 ms, against 214 to 315 ms', context: 'DuckDB SQL, top-10 templates: 40× to 50× faster' },
  { claim: 'On disk in DuckDB', result: '6.29 MB to 2.88 MB', context: '134,815 events: 0.46× the raw table' },
  { claim: 'Parquet on S3', result: '93.8% smaller than raw text', context: '20,000 events, 27.3% smaller than plain Parquet. Run against S3 emulators' },
];

export const compare = {
  cols: ['Drain3', 'CLP', 'Razobill'],
  rows: [
    { k: 'What it is', v: ['Template clustering in Python', 'Log compression and search system', 'Embedded engine and extension library'] },
    { k: 'Original line back', v: ['No. Variable tokens are discarded', 'Yes', 'Yes, byte for byte'] },
    { k: 'Stored as', v: ['In-memory parse tree', 'Custom archive (.clp, IR)', 'Standard columns: STRUCT, BIGINT[], VARCHAR[]'] },
    { k: 'Queried with', v: ['Python API', 'CLP command-line tools and FFI', 'SQL in DuckDB and ClickHouse, or Parquet'] },
    { k: 'Templates, median of 16 datasets', v: ['45.5', 'Not measured', '70.5'] },
    { k: 'Grouping accuracy, median', v: ['0.871', 'Not measured', '0.808'] },
    { k: 'Split speed, Drain3 test (five datasets)', v: ['3,779 to 28,000 lines/s', 'Not in this test', '57,060 to 81,270 lines/s: 2.5× to 15.1× faster'] },
    { k: 'Split speed, CLP test (four datasets)', v: ['Not in this test', '598k to 738k lines/s. CLP is faster', '245k to 332k lines/s'] },
    { k: 'Join speed', v: ['Not applicable', '332k to 405k lines/s', '530k to 593k lines/s'] },
    { k: 'Needle search, 100k lines', v: ['Not applicable', '216 to 299 ms', '2.7 to 8.8 ms'] },
    { k: 'Compressed size vs raw', v: ['Not applicable', '75.4% to 95.7% smaller', '77.0% to 94.9% smaller (Parquet, ZSTD)'] },
    { k: 'Choose it when', v: ['You only need the fewest templates and can drop the values', 'You need a standalone archive with command-line search', 'You query log values with SQL and must get every line back'] },
  ],
};

export const limits = [
  { t: 'It is not a clustering tool.', d: 'On the project’s 16-dataset test, median grouping accuracy was 0.808 for Razobill and 0.871 for Drain3. Razobill also keeps more templates (median 70.5 against 45.5). Pick Drain3 for the fewest templates, Razobill when you must get the text back.' },
  { t: 'The benchmarks are the author’s.', d: 'One machine, one operating system, no independent validation yet. Every script is in the repository. Please rerun them on your hardware.' },
  { t: 'S3 results come from emulators.', d: 'The dollar savings in the repository are modelled from assumed query volumes, not from a real AWS bill.' },
  { t: 'One line is capped at 16 MiB.', d: 'Larger inputs return an error (ValueError in Python, RZ_ERR_MESSAGE_TOO_LARGE in the C ABI).' },
  { t: 'WebAssembly and MinGW builds are excluded from the community distribution.', d: 'Pre-compiled community binaries cover Linux (x86_64, aarch64), macOS (Apple Silicon, Intel) and Windows MSVC (x86_64).' },
];

export const faq = [
  { q: 'What is Razobill?', a: 'Razobill is an open-source log parser written in C++20. It splits each log line into a template, numbers and words, stores them as columns, and joins them back into the exact original line on demand. It ships as a Python package called pyrazobill, a DuckDB extension and a C API.' },
  { q: 'Is it really lossless?', a: 'Yes, by design and by test. Every line in the project’s 69.6 million line audit was split, joined and compared to the original with zero mismatches. Empty lines, emoji and control characters round-trip too in pyrazobill 1.1.2.' },
  { q: 'How is Razobill different from Drain3?', a: 'Drain3 clusters similar lines and replaces the changing parts with a wildcard, so the original values are gone. Razobill uses a deterministic scanner and keeps the values, so every line can be joined back. In the project’s benchmarks it is faster, but Drain3 scored higher on median grouping accuracy.' },
  { q: 'How does Razobill compare to CLP?', a: 'YScope’s CLP (OSDI 2021) is a mature, lossless log management system built on custom archives. Both reach 100% byte-for-byte reconstruction with zero mismatches. CLP ingests faster (up to 738,000 lines/s in Python). Razobill outputs standard columns (STRUCT, BIGINT[], VARCHAR[]) that DuckDB and ClickHouse query with vectorized SQL: needle search ran 31× to 83× faster, and RzJoin restored lines in memory at 530k to 593k lines/s against 332k to 405k for CLP IR decode.' },
  { q: 'Do I need DuckDB to use it?', a: 'No. Use pyrazobill from Python alone, link the C++ library, or call the C API from another language. DuckDB is one of several integrations.' },
  { q: 'How do I install it?', a: 'In DuckDB, run INSTALL razobill FROM community; LOAD razobill;. In Python, run pip install pyrazobill. Wheels are published for Python 3.9 to 3.13 on Windows, Linux and macOS, and a pure-Python fallback is included.' },
  { q: 'What license is it under, and how do I cite it?', a: 'Apache License 2.0. If you use it in research, cite Atta (2026), Latitude and Doubt, Zenodo, DOI 10.5281/zenodo.23287286. The repository includes a NOTICE file and a BibTeX entry.' },
];

export const integrations = [
  { group: 'Databases', items: [
    { name: 'DuckDB', note: 'Scalar functions RzSplit and RzJoin inside SQL.', href: 'docs/how-to/duckdb/', logo: 'duckdb' },
    { name: 'ClickHouse and chDB', note: 'The template hash matches ClickHouse xxHash64, so keys agree across engines.', href: 'docs/how-to/chdb/', logo: 'clickhouse' },
  ] },
  { group: 'Files and storage', items: [
    { name: 'Parquet on S3', note: 'Stream the columns to Parquet with ZSTD. Tested on LocalStack and a mock S3 server.', href: 'docs/how-to/s3-compaction/' },
  ] },
  { group: 'Pipelines', items: [
    { name: 'OpenTelemetry', note: 'A Go collector processor that compacts logs before they leave the host.', href: 'docs/how-to/opentelemetry/' },
    { name: 'Vector', note: 'A Rust and VRL transform for edge agents.', href: 'docs/how-to/vector/' },
  ] },
  { group: 'Languages', items: [
    { name: 'Python 3.9 to 3.13', note: 'Wheels for Windows, Linux and macOS, with a pure-Python fallback.', href: 'docs/how-to/python/' },
    { name: 'C and C++20', note: 'Link librazobill directly, or use the plain extern "C" layer from Rust, Go, Zig, C# and Java.', href: 'docs/reference/c-abi/' },
  ] },
];

/**
 * Sidebar structure (Diátaxis, as in docs/README.md). Order inside a group is the array order.
 * A markdown file under docs/ that is not listed here still gets a page and appears under "More".
 */
export const groups = [
  { id: 'tutorials', title: 'Tutorials', lead: 'Learn by building something small.', ids: ['tutorials/getting-started', 'tutorials/first-pipeline'] },
  { id: 'how-to', title: 'How-to guides', lead: 'Solve a specific task.', ids: ['how-to/duckdb', 'how-to/chdb', 'how-to/python', 'how-to/opentelemetry', 'how-to/vector', 'how-to/s3-compaction', 'how-to/cpp-developer', 'how-to/neon-simd', 'how-to/troubleshooting'] },
  { id: 'reference', title: 'Reference', lead: 'Look up exact signatures and policies.', ids: ['reference/sql-functions', 'reference/python-api', 'reference/c-abi', 'reference/cpp-api', 'reference/compatibility', 'reference/packages', 'reference/glossary', 'reference/deprecation-policy', 'reference/release-governance'] },
  { id: 'explanation', title: 'Explanation', lead: 'Understand how it works and why.', ids: ['explanation/lossless-invertibility', 'explanation/architecture', 'explanation/comparisons', 'explanation/limits', 'explanation/benchmarks', 'explanation/faq', 'explanation/semantic-vs-exact', 'explanation/data-oriented-design', 'explanation/simd-vectorization', 'explanation/concurrency-model'] },
];
/** Not rendered as pages. docs/README.md is replaced by the designed index at /docs/. */
export const hidden = new Set(['readme']);
/** Pages that exist but belong with the project rather than a Diátaxis group. */
export const more = ['style'];

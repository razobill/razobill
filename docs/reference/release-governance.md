# Release governance and provenance

[Docs](../README.md) / [Reference](release-governance.md) / Release governance

This document specifies the verification gates, signing protocols, and distribution checklists required for official releases of Razobill.

- **Who this is for**: Release engineers, maintainers, and downstream packagers verifying build artifacts.
- **What you will have at the end**: An exact checklist of test gates, provenance requirements, and distribution channels.

---

## Release checklist

Every release candidate must complete this automated verification checklist before publication:

1. **Unit and regression test gates**:
   - All 59 C++ unit test cases must pass with zero failures (`build/UnitTests.exe`), verifying over 1.98 million assertions.
   - All 23 Python unit tests and 5 Hypothesis property tests must pass cleanly under target runtimes (`3.9` through `3.13`).
   - All 15 DuckDB extension integration tests must pass (`build/IntegrationDuckdb.exe`), verifying over 134,000 assertions.
   - Cargo integration tests in `ext/vector` and Go tests in `ext/otel` must pass.
2. **Quality and linting gates**:
   - `python tools/LintDocs.py README.md CONTRIBUTING.md docs/` must pass with zero errors.
   - `npx markdownlint-cli2 "**/*.md"` must report zero lint violations.
   - `python tools/VerifyMarkdownLinks.py` must verify that 100% of internal links resolve.
   - Python, C, and DuckDB surface checks must verify that public surfaces are within budget.
3. **Binary compatibility verification**:
   - Verify that `LIBRAZOBILL_ABI_VERSION` in `include/librazobill/Version.hpp` and `RZ_ABI_VERSION` in `include/librazobill/RazobillC.h` match the intended release contract.
   - Run `twine check lib/python/dist/*` to verify package metadata.

---

## Artifact distribution and provenance

Official release artifacts are distributed through three automated channels:

- **PyPI**: Pre-compiled binary wheels (`.whl`) and source distributions (`.tar.gz`) for `pyrazobill`.
- **DuckDB Community Extensions**: Signed loadable binary extensions (`razobill.duckdb_extension`) built via `extension-ci-tools`.
- **GitHub Releases**: Tagged release archives accompanied by SHA-256 checksums and SLSA build attestations (`.github/workflows/Release.yml`).

---

## Related

- [Deprecation policy](deprecation-policy.md): ABI stability and breaking change rules.
- [Compatibility matrix](compatibility.md): supported operating systems and Python versions.
- [Packages reference](packages.md): distributions and installation methods.

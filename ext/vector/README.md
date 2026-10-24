# Razobill Vector integration

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="../../assets/Logo.jpeg">
    <img src="../../assets/Logo-light.jpeg" alt="Razobill Logo" width="300">
  </picture>
</p>

High-throughput log compaction and telemetry transform for Vector (Datadog).
Powered by librazobill, it decomposes raw text events at up to 2.38 million lines per second per core.
It allows edge pipelines to ship structured columnar stripes directly to Amazon S3 or ClickHouse with zero byte loss.

---

## Quickstart pipeline

Configure the remap transform in your Vector pipeline (`vector.yaml`):

```yaml
transforms:
  razobill_transform:
    type: remap
    inputs:
      - container_logs
    source: |
      parsed = parse_razobill!(.message)
      .template = parsed.template
      .ints = parsed.ints
      .strs = parsed.strs
      del(.message)
```

---

## Building and testing

Compile the crate and run integration tests:

```bash
# 1. Build the native librazobill engine
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

# 2. Run Rust crate tests
cargo test --manifest-path ext/vector/Cargo.toml
```

---

## Documentation

- [Vector integration guide](../../docs/how-to/vector.md)
- [C ABI reference](../../docs/reference/c-abi.md)
- [Lossless invertibility](../../docs/explanation/lossless-invertibility.md)

# Publishing `pyrazobill` to PyPI

This guide provides the official release instructions for building, validating, and publishing the `pyrazobill 1.1.6` distribution packages to the Python Package Index (PyPI) and TestPyPI.

---

## 1. Prerequisites & Environment Setup

Ensure you have an isolated release environment with up-to-date packaging tools:

```bash
python -m pip install --upgrade pip build twine wheel cibuildwheel
```

Verify that your C++20 compiler toolchain is active:

- **Windows**: MSVC 2019+ or Clang 16+ with CMake 3.20+
- **Linux**: GCC 11+ or Clang 14+ (`manylinux_2_28` compatible)
- **macOS**: Apple Clang 14+ (Xcode 14+)

---

## 2. Building Local Source Distribution & Wheels

From the repository root or the `lib/python` directory, build both the source distribution (`.tar.gz`) and the platform wheel (`.whl`):

```bash
cd lib/python

# Clean previous build artifacts
rm -rf dist build *.egg-info pyrazobill/*.pyd pyrazobill/*.so

# Build source distribution and binary wheel
python -m build
```

Expected output in `lib/python/dist/`:

- `pyrazobill-1.1.7.tar.gz`: Universal source distribution bundled with C++20 core engine headers
- `pyrazobill-1.1.7-cp313-cp313-win_amd64.whl`: Platform-specific native binary wheel

---

## 3. Multi-Platform Wheels with `cibuildwheel`

For cross-platform binary distribution covering Windows, Linux (`manylinux_2_28`), and macOS:

```bash
# Run cibuildwheel locally or inside CI/CD matrix
python -m cibuildwheel --output-dir dist lib/python
```

### Supported Platform Matrix

| OS | Architecture | Target Tag | Minimum Toolchain |
| :--- | :--- | :--- | :--- |
| Windows | x86_64 | `cp310-cp313-win_amd64` | MSVC 19.29+ / MinGW-w64 GCC 12+ |
| Linux | x86_64 | `cp310-cp313-manylinux_2_28_x86_64` | GCC 11.2+ |
| Linux | aarch64 | `cp310-cp313-manylinux_2_28_aarch64` | GCC 11.2+ |
| macOS | x86_64 | `cp310-cp313-macosx_10_15_x86_64` | Apple Clang 13+ |
| macOS | arm64 | `cp310-cp313-macosx_11_0_arm64` | Apple Clang 13+ |

---

## 4. Validating the Distribution

Run `twine check` to ensure that metadata, licensing declarations, README formatting, and symbols conform strictly to PyPI standards:

```bash
twine check dist/*
```

Expected output:

```text
Checking dist/pyrazobill-1.1.7.tar.gz: PASSED
Checking dist/pyrazobill-1.1.7-cp313-cp313-win_amd64.whl: PASSED
```

---

## 5. Publishing to TestPyPI (Staging Verification)

Always test package publishing and installation in TestPyPI before a production release:

```bash
twine upload --repository testpypi dist/*
```

Verify installation and imports in a completely fresh, isolated virtual environment:

```bash
# Create and activate a clean test venv
python -m venv test_env
source test_env/bin/activate  # On Windows: test_env\Scripts\activate

# Install from TestPyPI with production PyPI fallback for dependencies
pip install --index-url https://test.pypi.org/simple/ --extra-index-url https://pypi.org/simple/ pyrazobill

# Verify runtime imports and lossless parsing
python -c "import pyrazobill as rz; rec = rz.RzSplit('User 1042 authenticated'); assert rz.RzJoin(*rec) == 'User 1042 authenticated'; print('TestPyPI Verification Passed! Version:', rz.__version__)"
```

---

## 6. Publishing to Production PyPI

Once verified against TestPyPI, publish the distribution artifacts to PyPI:

```bash
twine upload dist/*
```

Users worldwide can now install the official release directly via:

```bash
pip install pyrazobill
```

### Post-Release Smoke Test

```bash
python -c "import pyrazobill as rz; print('pyrazobill successfully installed. Version:', rz.__version__)"
```

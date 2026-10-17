# ==============================================================================
# pyrazobill setup and C++20 extension build script
#
# compiles native pyrazobill.Core extension from C++ source files; provides
# Windows SDK path auto-detection and resilient pure-Python fallback on compiler
# failure or missing pybind11 headers
# ==============================================================================

"""setup.py for pyrazobill

compiles native pyrazobill.Core C++20 extension module with AVX2 SIMD
acceleration and pure-Python fallback
"""

import sys
import os
import glob
import shutil
from pathlib import Path
from setuptools import setup

baseDir = Path(__file__).resolve().parent
repoRoot = baseDir.parent.parent

# sync staged core engine sources and headers when in monorepo layout so that
# isolated build environments (pip wheel, cibuildwheel, sdist) always bundle
# complete C++20 engine sources
srcCore = baseDir / "srcCore"
includeCore = baseDir / "includeCore"
thirdpartyCore = baseDir / "thirdpartyCore"

if (repoRoot / "src").exists() and (repoRoot / "include").exists():
    try:
        shutil.copytree(repoRoot / "src", srcCore, dirs_exist_ok=True)
        shutil.copytree(repoRoot / "include", includeCore, dirs_exist_ok=True)
        thirdpartyCore.mkdir(parents=True, exist_ok=True)
        if (repoRoot / "thirdparty" / "adbc").exists():
            shutil.copytree(repoRoot / "thirdparty" / "adbc", thirdpartyCore / "adbc", dirs_exist_ok=True)
        if (repoRoot / "thirdparty" / "duckdbcapi").exists():
            shutil.copytree(repoRoot / "thirdparty" / "duckdbcapi", thirdpartyCore / "duckdbcapi", dirs_exist_ok=True)
    except Exception:
        pass

if srcCore.exists() and includeCore.exists():
    sources = [
        "PyRazobill.cpp",
        "srcCore/Slots.cpp",
        "srcCore/Stats.cpp",
        "srcCore/Channel.cpp",
        "srcCore/Dialect.cpp",
        "srcCore/BackendAdbc.cpp",
        "srcCore/Razobill.cpp",
        "srcCore/RazobillC.cpp",
    ]
    includeDirs = [
        "includeCore",
        "thirdpartyCore/adbc",
        "thirdpartyCore/duckdbcapi",
    ]
elif (repoRoot / "src").exists() and (repoRoot / "include").exists():
    sources = [
        "PyRazobill.cpp",
        os.path.relpath(repoRoot / "src" / "Slots.cpp", baseDir).replace("\\", "/"),
        os.path.relpath(repoRoot / "src" / "Stats.cpp", baseDir).replace("\\", "/"),
        os.path.relpath(repoRoot / "src" / "Channel.cpp", baseDir).replace("\\", "/"),
        os.path.relpath(repoRoot / "src" / "Dialect.cpp", baseDir).replace("\\", "/"),
        os.path.relpath(repoRoot / "src" / "BackendAdbc.cpp", baseDir).replace("\\", "/"),
        os.path.relpath(repoRoot / "src" / "Razobill.cpp", baseDir).replace("\\", "/"),
        os.path.relpath(repoRoot / "src" / "RazobillC.cpp", baseDir).replace("\\", "/"),
    ]
    includeDirs = [
        os.path.relpath(repoRoot / "include", baseDir).replace("\\", "/"),
        os.path.relpath(repoRoot / "thirdparty" / "adbc", baseDir).replace("\\", "/"),
        os.path.relpath(repoRoot / "thirdparty" / "duckdbcapi", baseDir).replace("\\", "/"),
    ]
else:
    # standalone sdist package using staged core directory copies
    sources = [
        "PyRazobill.cpp",
        "srcCore/Slots.cpp",
        "srcCore/Stats.cpp",
        "srcCore/Channel.cpp",
        "srcCore/Dialect.cpp",
        "srcCore/BackendAdbc.cpp",
        "srcCore/Razobill.cpp",
        "srcCore/RazobillC.cpp",
    ]
    includeDirs = [
        "includeCore",
        "thirdpartyCore/adbc",
        "thirdpartyCore/duckdbcapi",
    ]


def find_windows_sdk_dirs():
    """discover Windows 10/11 SDK include and library directories

    in: sys.platform == 'win32'
    out: tuple of (include_directories_list, library_directories_list)
    thread: thread-safe, reads environment and filesystem
    """
    inc_dirs = []
    lib_dirs = []
    if sys.platform == "win32":
        # locate Windows Kits 10 header tree (ucrt, shared, um, winrt)
        pf86 = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
        kits_inc = os.path.join(pf86, "Windows Kits", "10", "Include")
        if os.path.exists(kits_inc):
            versions = sorted(glob.glob(os.path.join(kits_inc, "10.*")), reverse=True)
            if versions:
                latest = versions[0]
                for sub in ("ucrt", "shared", "um", "winrt"):
                    p = os.path.join(latest, sub)
                    if os.path.exists(p):
                        inc_dirs.append(p)
        # locate Windows Kits 10 library tree for target architecture
        kits_lib = os.path.join(pf86, "Windows Kits", "10", "Lib")
        if os.path.exists(kits_lib):
            versions = sorted(glob.glob(os.path.join(kits_lib, "10.*")), reverse=True)
            if versions:
                latest = versions[0]
                arch = "x64" if sys.maxsize > 2**32 else "x86"
                for sub in ("ucrt", "um"):
                    p = os.path.join(latest, sub, arch)
                    if os.path.exists(p):
                        lib_dirs.append(p)
    return inc_dirs, lib_dirs


try:
    from pybind11.setup_helpers import Pybind11Extension, build_ext

    class CustomBuildExt(build_ext):
        """custom extension builder injecting C++20 and SIMD architecture flags"""

        def build_extensions(self):
            compilerType = self.compiler.compiler_type
            inc_dirs, lib_dirs = find_windows_sdk_dirs()
            for ext in self.extensions:
                if compilerType == "msvc":
                    # enable C++20, AVX2 vectorization, synchronous exceptions,
                    # and 2^32 object sections
                    ext.extra_compile_args.extend(["/std:c++20", "/arch:AVX2", "/EHsc", "/bigobj"])
                    ext.include_dirs.extend(inc_dirs)
                    ext.library_dirs.extend(lib_dirs)
                else:
                    # enable C++20 and hidden symbol visibility
                    compile_args = ["-std=c++20", "-fvisibility=hidden"]
                    if os.environ.get("ENABLE_NATIVE_OPTIMIZATION"):
                        compile_args.append("-march=native")
                    ext.extra_compile_args.extend(compile_args)
                    if sys.platform != "win32":
                        ext.extra_link_args.append("-lpthread")
            try:
                super().build_extensions()
            except Exception as exc:
                if os.environ.get("CIBUILDWHEEL") or os.environ.get("PYRAZOBILL_REQUIRE_NATIVE"):
                    raise
                # fall back to pure-Python engine if compilation fails
                print(f"\n[pyrazobill] NOTICE: Native C++20 extension build failed ({exc}).")
                print("[pyrazobill] Proceeding with pure-Python fallback implementation.\n")
                self.extensions = []

    extModules = [
        Pybind11Extension(
            "pyrazobill.Core",
            sources=sources,
            include_dirs=includeDirs,
            cxx_std=20,
        ),
    ]
    cmdClass = {"build_ext": CustomBuildExt}
except ImportError:
    # pybind11 not installed at build time; build as pure-Python wheel
    extModules = []
    cmdClass = {}

setup(
    version="1.1.3",
    ext_modules=extModules,
    cmdclass=cmdClass,
)

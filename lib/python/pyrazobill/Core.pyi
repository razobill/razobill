# ==============================================================================
# pyrazobill C extension type stubs
#
# type definitions for C++ pybind11 / universal C ABI extension module (Core)
# defines split, template, join, split_many, hash, and version signatures
# ==============================================================================

from typing import Any, List, Sequence, Tuple, Union

# decompose log line into (template, ints, strs) tuple
def split(input: Union[str, bytes], mode: str = "semantic") -> Tuple[Union[str, bytes], List[int], List[Any]]: ...

# extract template discarding parameter token allocations
def template(input: Union[str, bytes], mode: str = "semantic") -> Union[str, bytes]: ...

# reconstruct original log message from template and parameter collections
def join(template: Union[str, bytes], ints: Sequence[int], strs: Sequence[Union[str, bytes]]) -> Union[str, bytes]: ...

# batch decompose multiple log lines releasing GIL across OpenMP worker threads
def split_many(
    inputs: Sequence[Union[str, bytes]], mode: str = "semantic"
) -> List[Tuple[Union[str, bytes], List[int], List[Any]]]: ...

# compute deterministic 64-bit xxHash64 (seed 0)
def hash(input: Union[str, bytes]) -> int: ...

# semantic package version string matching C++ core
def version() -> str: ...

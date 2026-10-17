# ==============================================================================
#  file : lib/python/pyrazobill/__init__.py
#  role : python package entrypoint, dynamic loader, and high-level api
#  inv  : join(split(x)) == x; bit-identical xxHash64 parity with c++ engine
# ==============================================================================

"""
Razobill: High-performance log parser and lossless line reconstruction engine.
==============================================================================

Split a log line into a template plus its numbers and words, and join them
back to the exact original bytes or string.

Quick Start:
------------
>>> import pyrazobill as rz
>>> line = "Failed password for root from 192.168.1.100 port 22"
>>> rec = rz.RzSplit(line)
>>> rec.template
'Failed password for {s} from {s} port {i}'
>>> rec.ints
[22]
>>> rec.strs
['root', '192.168.1.100']
>>> rz.RzJoin(*rec) == line
True
"""

import importlib
import os
import sys
from pathlib import Path
from typing import Iterable, List, NamedTuple, Optional, Sequence, Union

# -- dynamic library resolution on windows -------------------------------------

# on windows, add package directory and system PATH to DLL search path
_pkg_dir = Path(__file__).resolve().parent
if sys.platform == "win32":
    try:
        os.add_dll_directory(str(_pkg_dir))
    except (AttributeError, OSError, ValueError):
        # ignore unsupported directories or platforms without add_dll_directory
        pass
    for _p in os.environ.get("PATH", "").split(os.pathsep):
        if _p and os.path.isdir(_p):
            try:
                os.add_dll_directory(_p)
            except (AttributeError, OSError, ValueError):
                # ignore invalid or inaccessible PATH entries
                pass

# -- exception hierarchy and data records --------------------------------------


class RzError(ValueError):
    """Raised on malformed input, invalid syntax, or mismatched arguments."""

    pass


class RzRecord(NamedTuple):
    """Decomposed log record containing a template and its variable parameters."""

    template: Union[str, bytes]
    ints: List[int]
    strs: List[Union[str, bytes]]

    def __repr__(self) -> str:
        t_repr = repr(self.template)
        if len(t_repr) > 60:
            t_repr = t_repr[:57] + "..."
        return f"<razobill.RzRecord template={t_repr} ints={len(self.ints)} strs={len(self.strs)}>"


# -- native extension loader and pure-python fallback --------------------------

try:
    _core = importlib.import_module(".Core", package=__name__)
    _NATIVE = True
except (ImportError, AttributeError):
    # ~ falls back to pure-Python engine if C extension is unavailable
    from . import _fallback as _core

    _NATIVE = False


def isNative() -> bool:
    """return True if native C++ SIMD extension is loaded

    out: True if native C++ extension is loaded, False on fallback
    thread: thread-safe, immutable module flag
    """
    return _NATIVE


# -- public decomposition and reconstruction api -------------------------------


def RzSplit(line: Union[str, bytes], mode: str = "semantic") -> RzRecord:
    """split a log line into a template plus its numbers and words

    inv: RzJoin(RzSplit(line)) == line
    in: line is str or bytes; mode is 'semantic' or 'exact'
    out: RzRecord namedtuple containing template, ints list, strs list
    own: returns newly allocated Python objects owning their memory
    thread: thread-safe; releases GIL in native C++ engine during parsing
    err: raises RzError on unsupported type or invalid scan mode

    Parameters
    ----------
    line : str or bytes
        log line to split; empty string or bytes are valid
    mode : {"semantic", "exact"}, default="semantic"
        token scanning behavior: "semantic" groups variable tokens (such as IPs,
        paths, and usernames) into string placeholders; "exact" isolates numbers

    Returns
    -------
    RzRecord
        named tuple of (template, ints, strs) where template contains typed
        placeholders ({i} for integers, {s} for strings); returns matching type
        (str for str input, bytes for bytes input)

    Examples
    --------
    >>> import pyrazobill as rz
    >>> rec = rz.RzSplit("Failed password for root from 192.168.1.100 port 22")
    >>> rec.template
    'Failed password for {s} from {s} port {i}'
    >>> rec.ints
    [22]
    >>> rec.strs
    ['root', '192.168.1.100']
    """
    if not isinstance(line, (str, bytes)):
        raise RzError(
            f"[razobill] What happened: Unsupported input type. "
            f"Why: Expected str or bytes, got {type(line).__name__}. "
            f"What to do: Pass a str or bytes object. "
            f"Offending input: {repr(line)[:60]}."
        )
    if mode not in ("semantic", "exact"):
        raise RzError(
            f"[razobill] What happened: Invalid scan mode. "
            f"Why: Expected 'semantic' or 'exact', got '{mode}'. "
            f"What to do: Use mode='semantic' (default) or mode='exact'. "
            f"Offending input: '{mode}'."
        )
    try:
        # delegate to native c++ extension or pure-python fallback
        t, i, s = _core.split(line, mode)
        return RzRecord(t, i, s)
    except RzError:
        raise
    except (ValueError, TypeError) as exc:
        raise RzError(str(exc)) from exc


def RzJoin(
    template: Union[str, bytes, RzRecord],
    ints: Optional[Sequence[int]] = None,
    strs: Optional[Sequence[Union[str, bytes]]] = None,
) -> Union[str, bytes]:
    """join a template and its parameters back to exact original bytes or string

    inv: RzJoin(*RzSplit(line)) == line and RzJoin(RzSplit(line)) == line
    in: template is str, bytes, or RzRecord; ints is sequence of int;
        strs is sequence of str or bytes
    out: reconstructed string or byte string matching template type
    own: returns newly allocated Python str or bytes owning its memory
    thread: thread-safe; releases GIL in native C++ engine during join
    err: raises RzError on mismatched placeholder counts or invalid types

    Parameters
    ----------
    template : str, bytes, or RzRecord
        template containing {i} and {s} placeholders, or an RzRecord
    ints : sequence of int, optional
        integers corresponding to {i} placeholders
    strs : sequence of str or bytes, optional
        strings corresponding to {s} placeholders

    Returns
    -------
    str or bytes
        reconstructed line, matching the type of the template

    Examples
    --------
    >>> import pyrazobill as rz
    >>> line = "Failed password for root from 192.168.1.100 port 22"
    >>> rec = rz.RzSplit(line)
    >>> rz.RzJoin(*rec) == line
    True
    """
    # unpack RzRecord or 3-tuple if passed as first argument
    if hasattr(template, "template") and hasattr(template, "ints") and hasattr(template, "strs"):
        template, ints, strs = template.template, template.ints, template.strs
    elif isinstance(template, tuple) and len(template) == 3 and ints is None and strs is None:
        template, ints, strs = template[0], template[1], template[2]

    # treat omitted parameter collections as empty sequences
    if ints is None:
        ints = []
    if strs is None:
        strs = []

    if not isinstance(template, (str, bytes)):
        raise RzError(
            f"[razobill] What happened: Unsupported template type. "
            f"Why: Expected str or bytes, got {type(template).__name__}. "
            f"What to do: Pass a str, bytes, or RzRecord object."
        )

    try:
        return _core.join(template, ints, strs)
    except RzError:
        raise
    except (ValueError, TypeError) as exc:
        raise RzError(str(exc)) from exc


def RzTemplate(line: Union[str, bytes], mode: str = "semantic") -> Union[str, bytes]:
    """extract structural template for a log line without parameter lists

    inv: template matches RzSplit(line).template byte-for-byte
    in: line is str or bytes; mode is 'semantic' or 'exact'
    out: template string or bytes containing {i} and {s} placeholders
    own: returns newly allocated Python str or bytes owning its memory
    thread: thread-safe; releases GIL in native C++ engine
    err: raises RzError on unsupported type or invalid scan mode

    Parameters
    ----------
    line : str or bytes
        log line to extract template from
    mode : {"semantic", "exact"}, default="semantic"
        token scanning behavior

    Returns
    -------
    str or bytes
        template with {i} and {s} placeholders
    """
    if not isinstance(line, (str, bytes)):
        raise RzError(
            f"[razobill] What happened: Unsupported input type. "
            f"Why: Expected str or bytes, got {type(line).__name__}. "
            f"What to do: Pass a str or bytes object."
        )
    if mode not in ("semantic", "exact"):
        raise RzError(
            f"[razobill] What happened: Invalid scan mode. "
            f"Why: Expected 'semantic' or 'exact', got '{mode}'. "
            f"What to do: Use mode='semantic' (default) or mode='exact'."
        )
    try:
        return _core.template(line, mode)
    except RzError:
        raise
    except (ValueError, TypeError) as exc:
        raise RzError(str(exc)) from exc


def RzSplitMany(lines: Iterable[Union[str, bytes]], mode: str = "semantic") -> List[RzRecord]:
    """split an iterable of log lines in batch, releasing GIL across cores

    inv: all(RzJoin(r) == l for r, l in zip(RzSplitMany(lines), lines))
    in: lines is iterable of str or bytes; mode is 'semantic' or 'exact'
    out: list of RzRecord namedtuples matching input order
    own: returns newly allocated Python list and record objects
    thread: thread-safe; releases GIL in native C++ engine across OpenMP threads
    err: raises RzError on non-str/bytes item or invalid scan mode

    Parameters
    ----------
    lines : iterable of str or bytes
        log lines to split
    mode : {"semantic", "exact"}, default="semantic"
        token scanning behavior

    Returns
    -------
    list of RzRecord
        decomposed records for all input lines
    """
    if mode not in ("semantic", "exact"):
        raise RzError(
            f"[razobill] What happened: Invalid scan mode. "
            f"Why: Expected 'semantic' or 'exact', got '{mode}'. "
            f"What to do: Use mode='semantic' (default) or mode='exact'."
        )
    line_list = list(lines)
    for i, line in enumerate(line_list):
        if not isinstance(line, (str, bytes)):
            raise RzError(
                f"[razobill] What happened: Unsupported input type at index {i}. "
                f"Why: Expected str or bytes, got {type(line).__name__}. "
                f"What to do: Ensure all items in lines are str or bytes."
            )
    try:
        raw_records = _core.split_many(line_list, mode)
        return [RzRecord(t, ints, strs) for t, ints, strs in raw_records]
    except RzError:
        raise
    except (ValueError, TypeError) as exc:
        raise RzError(str(exc)) from exc


def RzHash(line: Union[str, bytes]) -> int:
    """compute deterministic 64-bit template or message hash via xxHash64 (seed 0)

    inv: bit-identical to DuckDB RzHash() and ClickHouse xxHash64()
    in: line is str or bytes
    out: 64-bit unsigned integer hash [0, 2^64 - 1]
    own: returns immutable Python integer
    thread: thread-safe; calls xxHash64 with zero shared state
    err: raises RzError on non-str/bytes input

    Parameters
    ----------
    line : str or bytes
        template or message string or bytes to hash

    Returns
    -------
    int
        64-bit unsigned integer hash

    Examples
    --------
    >>> import pyrazobill as rz
    >>> rec = rz.RzSplit("Failed password for root from 192.168.1.100 port 22")
    >>> rz.RzHash(rec.template)
    54855824174769941
    """
    if not isinstance(line, (str, bytes)):
        raise RzError(
            f"[razobill] What happened: Unsupported input type. "
            f"Why: Expected str or bytes, got {type(line).__name__}. "
            f"What to do: Pass a str or bytes object. "
            f"Offending input: {repr(line)[:60]}."
        )
    try:
        return _core.hash(line)
    except RzError:
        raise
    except (ValueError, TypeError) as exc:
        raise RzError(str(exc)) from exc


# -- package metadata and public symbols ---------------------------------------

__version__ = "1.1.2"

# public export surface pinned to <= 8 symbols per TestPyRazobill compliance
__all__ = [
    "RzSplit",
    "RzJoin",
    "RzTemplate",
    "RzHash",
    "RzSplitMany",
    "RzRecord",
    "RzError",
    "__version__",
]

# -- dynamic alias resolution -------------------------------------------------

# ergonomic lowercase aliases resolved dynamically to keep __all__ compact
_ALIASES = {
    "split": RzSplit,
    "join": RzJoin,
    "template": RzTemplate,
    "hash": RzHash,
    "split_many": RzSplitMany,
    "Record": RzRecord,
    "Error": RzError,
    "isNative": isNative,
}


def __getattr__(name: str):
    """resolve lowercase export aliases without bloating module namespace"""
    if name in _ALIASES:
        return _ALIASES[name]
    raise AttributeError(f"module '{__name__}' has no attribute '{name}'")


def __dir__():
    """include dynamic aliases in interactive tab completion and dir() listing"""
    return sorted(list(globals().keys()) + list(_ALIASES.keys()))

# ==============================================================================
# pyrazobill pure-python fallback engine
#
# pure-Python mirror of librazobill C++20 engine and C ABI; active when native
# extension is not compiled or loadable; guarantees exact output parity with
# C++ SIMD engine across all inputs (str and bytes)
#
# invariants:
#   1. exact invertibility: join(*split(x)) == x for all str and bytes
#   2. bit-parity: hash(x) == xxHash64(x, seed 0) matching C++, DuckDB, chDB
#   3. type preservation: str input -> str template/strs; bytes -> bytes
# ==============================================================================

"""pure-Python fallback implementation for pyrazobill

provides equivalent functionality when the native C++20 extension is
unavailable; guarantees exact byte-for-byte invertibility and xxHash64 parity
"""

from typing import Any, List, Sequence, Tuple, Union

# structural delimiter set matching Slots.h whitespace and punctuation
DELIMITERS = set("\t\n\x0c\r =,;:()[]{}\"'<>|/")
# anchor words triggering string token promotion in semantic mode
CONTEXT_KEYWORDS = {
    "for",
    "user",
    "by",
    "from",
    "ruser",
    "rhost",
    "host",
    "account",
    "client",
    "src",
    "dest",
    "to",
    "at",
    "proxy",
    "server",
    "node",
    "target",
    "app",
    "device",
}


def _is_int_token(s: str) -> bool:
    """check if token is a valid non-overflowing base-10 integer

    inv: parses into int64 without overflow or leading zero ambiguity
    in: s is ASCII candidate string
    out: True if s is 1-18 decimal digits and no leading zero (except "0")
    thread: thread-safe, pure function
    """
    # reject empty strings and tokens > 18 digits to prevent int64 overflow
    if not s or len(s) > 18:
        return False
    # preserve leading zero strings (e.g. "0123") as strings to prevent loss
    if s[0] == "0":
        return len(s) == 1
    return all("0" <= c <= "9" for c in s)


def _scan(msg: str, mode: str):
    """scan message and yield alternating gap slices and variable tokens

    in: msg is decoded str; mode is 'semantic' or 'exact'
    out: generator yielding (gap_slice, token_or_None) pairs
    thread: thread-safe generator
    """
    # ~ semantic mode classifies identifiers and paths into string slots
    n = len(msg)
    p = 0
    gap = 0
    last_word = ""
    unclosed_dquote_end = -1
    unclosed_squote_end = -1

    while p < n:
        d_start = p
        # skip delimiters; keep quotes in semantic mode to parse string literals
        while p < n and msg[p] in DELIMITERS and not (mode == "semantic" and msg[p] in ('"', "'")):
            p += 1
        d_end = p
        last_delim = msg[d_end - 1] if d_end > d_start else ""

        if p >= n:
            break

        # handle quoted string literals in semantic mode
        if mode == "semantic" and msg[p] in ('"', "'"):
            q = msg[p]
            if (q == '"' and p < unclosed_dquote_end) or (q == "'" and p < unclosed_squote_end):
                p += 1
                continue
            else:
                q_start = p
                p += 1
                while p < n and msg[p] != q and msg[p] != "\n":
                    if msg[p] == "\\" and p + 1 < n:
                        p += 2
                    else:
                        p += 1
                if p < n and msg[p] == q:
                    p += 1
                    tok = msg[q_start:p]
                    yield msg[gap:q_start], tok
                    gap = p
                    last_word = ""
                    continue
                else:
                    # track unclosed quote to avoid quadratic rescanning
                    if q == '"':
                        unclosed_dquote_end = p
                    else:
                        unclosed_squote_end = p
                    p = q_start + 1
                    continue

        # scan contiguous non-delimiter token
        start = p
        seen_digit = False
        while p < n and msg[p] not in DELIMITERS:
            if "0" <= msg[p] <= "9":
                seen_digit = True
            p += 1

        tok = msg[start:p]
        is_var = False

        if seen_digit:
            # any token containing digits is treated as variable ({i} or {s})
            is_var = True
        elif mode == "semantic":
            # semantic mode promotes tokens following key delimiters or keywords
            if last_delim in ("=", "@", '"', "'"):
                is_var = True
            elif last_word and last_word.lower() in CONTEXT_KEYWORDS:
                is_var = True
            elif p < n and msg[p] == ":" and "." in tok:
                is_var = True
            elif len(tok) > 4 and (tok.endswith(".exe") or tok.endswith(".dll") or tok.endswith(".so")):
                is_var = True

        if is_var:
            yield msg[gap:start], tok
            gap = p
            last_word = ""
        else:
            last_word = tok

    # flush trailing gap slice
    if gap < n:
        yield msg[gap:n], None


def _escape_braces(s: str) -> str:
    """escape literal curly braces to prevent collision with template slots

    inv: every '{' becomes '{{' and '}' becomes '}}'
    in: s is raw text slice
    out: escaped string
    """
    return s.replace("{", "{{").replace("}", "}}")


def split(input_val: Union[str, bytes], mode: str = "semantic") -> Tuple[Union[str, bytes], List[int], List[Any]]:
    """decompose log line into template, integer list, and string list

    inv: join(*split(x)) == x byte-for-byte
    in: input_val is str or bytes; mode is 'semantic' or 'exact'
    out: tuple of (template, ints, strs)
    own: returns newly allocated tuple and lists
    thread: thread-safe, no shared state
    err: raises TypeError on non-str/bytes; ValueError on unknown mode
    """
    if mode not in ("semantic", "exact"):
        raise ValueError(
            f"[razobill] What happened: Invalid scan mode. "
            f"Why: Expected 'semantic' or 'exact', got '{mode}'. "
            f"What to do: Pass mode='semantic' (default) or mode='exact'."
        )

    # latin1 maps bytes 0x00..0xFF to Unicode code points 1-to-1 without loss
    is_bytes = isinstance(input_val, bytes)
    if is_bytes:
        msg_str = input_val.decode("latin1")
    elif isinstance(input_val, str):
        msg_str = input_val
    else:
        raise TypeError(f"Expected str or bytes, got {type(input_val).__name__}")

    if not msg_str:
        return (b"" if is_bytes else "", [], [])

    tmpl_parts = []
    ints: List[int] = []
    strs: List[Any] = []

    for gap, tok in _scan(msg_str, mode):
        tmpl_parts.append(_escape_braces(gap))
        if tok is not None:
            if _is_int_token(tok):
                tmpl_parts.append("{i}")
                ints.append(int(tok))
            else:
                tmpl_parts.append("{s}")
                if is_bytes:
                    strs.append(tok.encode("latin1"))
                else:
                    strs.append(tok)

    tmpl_str = "".join(tmpl_parts)
    tmpl_res: Union[str, bytes] = tmpl_str.encode("latin1") if is_bytes else tmpl_str
    return (tmpl_res, ints, strs)


def template(input_val: Union[str, bytes], mode: str = "semantic") -> Union[str, bytes]:
    """extract template from log line discarding parameter tokens

    inv: matches split(x)[0] exactly
    in: input_val is str or bytes; mode is 'semantic' or 'exact'
    out: template string or bytes
    thread: thread-safe
    """
    t, _, _ = split(input_val, mode)
    return t


def join(template_val: Union[str, bytes], ints: Sequence[int], strs: Sequence[Union[str, bytes]]) -> Union[str, bytes]:
    """reconstruct original log message from template and parameter collections

    inv: join(*split(x)) == x
    in: template_val is str or bytes; ints and strs sequences match slots
    out: reconstructed str or bytes matching template type
    own: returns newly allocated str or bytes
    thread: thread-safe, no shared state
    err: raises ValueError on placeholder count mismatch or malformed syntax;
         TypeError on invalid template type
    """
    is_bytes = isinstance(template_val, bytes)
    if is_bytes:
        tmpl_str = template_val.decode("latin1")
        str_tokens = [s.decode("latin1") if isinstance(s, bytes) else str(s) for s in strs]
    elif isinstance(template_val, str):
        tmpl_str = template_val
        str_tokens = [s.decode("utf-8", "surrogateescape") if isinstance(s, bytes) else str(s) for s in strs]
    else:
        raise TypeError(f"Expected str or bytes template, got {type(template_val).__name__}")

    out_parts = []
    i_idx = 0
    s_idx = 0
    p = 0
    n = len(tmpl_str)

    while p < n:
        c = tmpl_str[p]
        if c == "{":
            # unescape literal double brace '{{' -> '{'
            if p + 1 < n and tmpl_str[p + 1] == "{":
                out_parts.append("{")
                p += 2
            # substitute integer slot '{i}' with formatted integer
            elif p + 2 < n and tmpl_str[p + 1] == "i" and tmpl_str[p + 2] == "}":
                if i_idx >= len(ints):
                    raise ValueError(
                        "[razobill] What happened: Failed to join log components. "
                        "Why: Count mismatch (template has more {i} placeholders than provided ints). "
                        "What to do: Verify that {i} and {s} placeholders match provided values. "
                        f"Offending template: '{tmpl_str[:80]}'."
                    )
                out_parts.append(str(ints[i_idx]))
                i_idx += 1
                p += 3
            # substitute string slot '{s}' with parameter string
            elif p + 2 < n and tmpl_str[p + 1] == "s" and tmpl_str[p + 2] == "}":
                if s_idx >= len(str_tokens):
                    raise ValueError(
                        "[razobill] What happened: Failed to join log components. "
                        "Why: Count mismatch (template has more {s} placeholders than provided strs). "
                        "What to do: Verify that {i} and {s} placeholders match provided values. "
                        f"Offending template: '{tmpl_str[:80]}'."
                    )
                out_parts.append(str_tokens[s_idx])
                s_idx += 1
                p += 3
            else:
                raise ValueError(
                    f"[razobill] What happened: Malformed template syntax. "
                    f"Why: Unclosed or unrecognized placeholder near index {p}. "
                    f"What to do: Use {{i}}, {{s}}, or escape literal braces as '{{{{' or '}}}}'."
                )
        elif c == "}":
            # unescape literal double brace '}}' -> '}'
            if p + 1 < n and tmpl_str[p + 1] == "}":
                out_parts.append("}")
                p += 2
            else:
                raise ValueError(
                    f"[razobill] What happened: Malformed template syntax. "
                    f"Why: Unexpected single '}}' at index {p}. "
                    f"What to do: Escape literal braces as '}}}}'."
                )
        else:
            out_parts.append(c)
            p += 1

    # ensure every provided parameter was consumed by the template
    if i_idx != len(ints) or s_idx != len(strs):
        raise ValueError(
            f"[razobill] What happened: Failed to join log components. "
            f"Why: Count mismatch (provided {len(ints)} ints, {len(strs)} strs, but template used {i_idx} ints, {s_idx} strs). "
            f"What to do: Verify that template placeholders match provided values. "
            f"Offending template: '{tmpl_str[:80]}'."
        )

    res_str = "".join(out_parts)
    return res_str.encode("latin1") if is_bytes else res_str


def split_many(
    inputs: Sequence[Union[str, bytes]], mode: str = "semantic"
) -> List[Tuple[Union[str, bytes], List[int], List[Any]]]:
    """batch decompose multiple log lines sequentially

    in: inputs is sequence of str or bytes; mode is 'semantic' or 'exact'
    out: list of (template, ints, strs) tuples
    thread: thread-safe
    """
    return [split(item, mode) for item in inputs]


def hash(input_val: Union[str, bytes]) -> int:
    """compute deterministic 64-bit xxHash64 (seed 0) of string or bytes

    inv: bit-identical to C++ RzHash, DuckDB RzHash, and ClickHouse xxHash64
    in: input_val is str or bytes
    out: 64-bit unsigned integer in [0, 2^64 - 1]
    thread: thread-safe, zero shared state
    err: raises TypeError on non-str/bytes input
    """
    if isinstance(input_val, str):
        data = input_val.encode("utf-8", "surrogateescape")
    elif isinstance(input_val, bytes):
        data = input_val
    else:
        raise TypeError(
            f"[razobill] What happened: Unsupported input type in hash. "
            f"Why: Expected str or bytes, got {type(input_val).__name__}. "
            f"What to do: Pass str or bytes."
        )

    # fast path using C-accelerated xxhash if installed in environment
    try:
        import xxhash

        return xxhash.xxh64_intdigest(data, seed=0)
    except ImportError:
        # Fall back to pure-Python xxHash64 when xxhash is not installed
        pass

    import struct

    # xxHash64 algorithm constants pinned to official Yann Collet reference
    P1 = 11400714785074694791
    P2 = 14029467366897019727
    P3 = 1609587929392839161
    P4 = 9650029242287828579
    P5 = 2870177450012600261
    MASK64 = 0xFFFFFFFFFFFFFFFF

    def rotl(x: int, r: int) -> int:
        return ((x << r) & MASK64) | (x >> (64 - r))

    def round_fn(acc: int, v: int) -> int:
        return (rotl((acc + v * P2) & MASK64, 31) * P1) & MASK64

    def merge_fn(h_val: int, v: int) -> int:
        return ((h_val ^ round_fn(0, v)) * P1 + P4) & MASK64

    n = len(data)
    p = 0
    # process 32-byte stripes with four 64-bit state accumulators
    if n >= 32:
        v1 = (P1 + P2) & MASK64
        v2 = P2
        v3 = 0
        v4 = (0 - P1) & MASK64
        limit = n - 32
        while p <= limit:
            k1, k2, k3, k4 = struct.unpack_from("<QQQQ", data, p)
            v1 = round_fn(v1, k1)
            v2 = round_fn(v2, k2)
            v3 = round_fn(v3, k3)
            v4 = round_fn(v4, k4)
            p += 32
        h_acc = (rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18)) & MASK64
        h_acc = merge_fn(h_acc, v1)
        h_acc = merge_fn(h_acc, v2)
        h_acc = merge_fn(h_acc, v3)
        h_acc = merge_fn(h_acc, v4)
    else:
        h_acc = P5

    h_acc = (h_acc + n) & MASK64

    # process remaining 8-byte blocks
    while p + 8 <= n:
        (k1,) = struct.unpack_from("<Q", data, p)
        k1 = round_fn(0, k1)
        h_acc = (rotl(h_acc ^ k1, 27) * P1 + P4) & MASK64
        p += 8

    # process remaining 4-byte block
    if p + 4 <= n:
        (k1,) = struct.unpack_from("<I", data, p)
        h_acc = (rotl(h_acc ^ ((k1 * P1) & MASK64), 23) * P2 + P3) & MASK64
        p += 4

    # process remaining individual bytes
    while p < n:
        k1 = data[p]
        h_acc = (rotl(h_acc ^ ((k1 * P5) & MASK64), 11) * P1) & MASK64
        p += 1

    # avalanche finalizer bit mixer
    h_acc ^= h_acc >> 33
    h_acc = (h_acc * P2) & MASK64
    h_acc ^= h_acc >> 29
    h_acc = (h_acc * P3) & MASK64
    h_acc ^= h_acc >> 32

    return h_acc & MASK64


def version() -> str:
    """return semantic package version string matching C++ core"""
    return "1.1.2"

#!/usr/bin/env python3
# =============================================================================
# documentation style, structure, and glossary linter
#
# validates Markdown files against docs/STYLE.md: untagged code blocks,
# marketing hype words, deprecated symbols, sentence-case headings, and length
# ~ scans documentation to reject marketing fluff and banned buzzwords
# =============================================================================

import sys
import re
from pathlib import Path

# Banned terms and marketing hype
BANNED_WORDS = {
    r"\bblazing\b": "Do not use hype word 'blazing'.",
    r"\brevolutionary\b": "Do not use hype word 'revolutionary'.",
    r"\bseamless\b": "Do not use hype word 'seamless'.",
    r"\beffortless\b": "Do not use hype word 'effortless'.",
    r"\brobust\b": "Do not use hype word 'robust'.",
    r"\bpowerful\b": "Do not use hype word 'powerful'.",
    r"\bworld-class\b": "Do not use hype word 'world-class'.",
    r"let's dive in": "Do not use conversational filler 'let\\'s dive in'.",
    r"in today's world": "Do not use conversational filler 'in today\\'s world'.",
}

# Legacy symbols that must not appear in modern guides or reference
DEPRECATED_SYMBOLS = {
    r"\brbDecompose\b": "Use RzSplit instead.",
    r"\brbForm\b": "Use RzTemplate or RzSplit().template instead.",
    r"\brbInts\b": "Use RzSplit().ints instead.",
    r"\brbStrs\b": "Use RzSplit().strs instead.",
    r"\brbReconstruct\b": "Use RzJoin instead.",
    r"\brbRebuild\b": "Use RzJoin instead.",
    r"\brbEntropy\b": "Legacy metric removed from extension.",
    r"\bRzParse\b": "Use RzSplit instead.",
    r"\bRzReconstructSlots\b": "Use RzJoin instead.",
    r"\bRzTemplateOf\b": "Use RzTemplate instead.",
    r"\\x12": "Use {s} placeholder instead of literal \\x12 delimiter.",
    r"\\x13": "Use {i} placeholder instead of literal \\x13 delimiter.",
}

# Allowlist for files that document deprecations or historical migrations
DEPRECATION_ALLOWLIST = {
    "CHANGELOG.md",
    "docs/governance/DeprecationPolicy.md",
    "docs/STYLE.md",
    "AUDIT.md",
}


def lint_file(file_path: Path) -> list:
    """audit a markdown file against docs/STYLE.md rules.

    inv: checks do not mutate file contents
    in: file_path points to an existing readable markdown file
    out: list of violation strings, empty if file complies
    """
    errors = []
    text = file_path.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()
    rel_path = file_path.as_posix()

    # 1. Untagged code fences
    inside_fence = False
    for i, line in enumerate(lines, 1):
        if line.startswith("```"):
            if not inside_fence:
                tag = line[3:].strip()
                if not tag:
                    errors.append(f"{rel_path}:{i} Untagged code fence. Explicit language required.")
                inside_fence = True
            else:
                inside_fence = False

    # 2. Banned hype words
    for pat, msg in BANNED_WORDS.items():
        for i, line in enumerate(lines, 1):
            if re.search(pat, line, re.IGNORECASE):
                # ignore if inside markdown rules quoting the word itself
                # or historical changelog
                if any(k in rel_path for k in ["STYLE.md", "LintDocs.py", "CONTRIBUTING.md", "CHANGELOG.md"]):
                    continue
                errors.append(f"{rel_path}:{i} Banned word detected ({pat}): {msg}")

    # 3. Deprecated symbols check
    if rel_path not in DEPRECATION_ALLOWLIST:
        for pat, msg in DEPRECATED_SYMBOLS.items():
            for i, line in enumerate(lines, 1):
                if re.search(pat, line):
                    errors.append(f"{rel_path}:{i} Deprecated symbol ({pat}): {msg}")

    # 4. Sentence-case headings (ignoring allowed acronyms and code literals)
    ALLOWED_CAPS = {
        "A",
        "B",
        "C",
        "D",
        "C++",
        "C++20",
        "DuckDB",
        "Python",
        "SIMD",
        "OTel",
        "OpenTelemetry",
        "Vector",
        "API",
        "ABI",
        "ADBC",
        "SQL",
        "CLI",
        "S3",
        "AWS",
        "JSD",
        "TL;DR",
        "AVX-512",
        "AVX2",
        "NEON",
        "CPU",
        "RAM",
        "Parquet",
        "ZSTD",
        "JSON",
        "Razobill",
        "I/O",
        "POSIX",
        "CMake",
        "PyPI",
        "SDK",
        "UBIGINT",
        "VARCHAR",
        "ClickHouse",
        "chDB",
        "MergeTree",
        "CLP",
        "IR",
    }

    heading_pattern = re.compile(r"^(#{1,6})\s+(.*)$")
    inside_fence_for_heading = False
    for i, line in enumerate(lines, 1):
        if line.startswith("```"):
            inside_fence_for_heading = not inside_fence_for_heading
            continue
        if inside_fence_for_heading:
            continue
        m = heading_pattern.match(line)
        if m:
            heading_text = m.group(2).strip()
            clean_heading = re.sub(
                r"^(Step\s+\d+:?|\d+(\.\d+)*\.?|[A-Za-z]\.)\s+", "", heading_text, flags=re.IGNORECASE
            )
            # Remove inline code blocks `...` completely
            clean_heading = re.sub(r"`[^`]*`", "", clean_heading).strip()
            words = clean_heading.split()
            if len(words) > 1:
                # Check for Title Case where small words or non-initial
                # words are capitalized
                capitalized_non_first = 0
                for w in words[1:]:
                    clean_w = re.sub(r"[^\w+]", "", w)
                    if clean_w and clean_w[0].isupper() and clean_w not in ALLOWED_CAPS:
                        capitalized_non_first += 1
                if capitalized_non_first >= 2:
                    errors.append(
                        f"{rel_path}:{i} Heading '{heading_text}' appears to be Title Case instead of sentence-case."
                    )

    # 5. Sentence length check (ignoring code fences and tables)
    inside_code = False
    sentences = []
    for line in lines:
        if line.startswith("```"):
            inside_code = not inside_code
            continue
        if inside_code or line.startswith("|") or line.startswith("#") or not line.strip():
            continue
        # Split sentences roughly by . ! ?
        for s in re.split(r"[.!?]+", line):
            words = s.strip().split()
            if len(words) > 0:
                sentences.append(len(words))

    if sentences:
        avg_sentence_len = sum(sentences) / len(sentences)
        if avg_sentence_len > 25:
            errors.append(
                f"{rel_path} Average sentence length is too long ({avg_sentence_len:.1f} words > 25.0 threshold)."
            )

    return errors


def main():
    """run documentation style linter across paths from argv or defaults.

    err: exits with status code 1 if any style errors are detected
    """
    paths = sys.argv[1:] if len(sys.argv) > 1 else ["README.md", "docs/STYLE.md"]
    total_errors = []
    for p_str in paths:
        p = Path(p_str)
        if p.is_file():
            total_errors.extend(lint_file(p))
        elif p.is_dir():
            for sub_p in p.rglob("*.md"):
                if "superpowers" in sub_p.parts:
                    continue
                total_errors.extend(lint_file(sub_p))

    if total_errors:
        print(f"FAILED: Found {len(total_errors)} lint issue(s):")
        for err in total_errors:
            print(f"  - {err}")
        sys.exit(1)
    else:
        print("PASSED: All checked documentation files comply with docs/STYLE.md.")
        sys.exit(0)


if __name__ == "__main__":
    main()

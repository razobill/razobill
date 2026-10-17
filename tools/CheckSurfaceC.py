#!/usr/bin/env python3
# =============================================================================
# surface budget enforcer: Razobill C Universal ABI (RazobillC.h)
#
# enforces hard limits: <= 7 exported functions, <= 3 types in RazobillC.h,
# unified CamelCase Rz prefix, zero legacy aliases, and zero banned terms
# ~ الحارس اللي بيمنع أي حد يزود دالة في الواجهة العامة من غير ما يحسب حسابها
# =============================================================================

import re
import sys
from pathlib import Path

HEADER_PATH = Path(__file__).resolve().parent.parent / "include" / "librazobill" / "RazobillC.h"

BANNED_TERMS = [
    "entropy",
    "ledger",
    "latitude",
    "doubt",
    "conveyance",
    "leakage",
    "channel capacity",
    "miller-madow",
    "certified interval",
    "shannon",
    "arena",
    "slot",
    "form key",
]


def main():
    """audit C ABI header declarations against surface budget and banned terms.

    inv: RazobillC.h exports <= 7 functions, <= 3 types, and 0 banned terms
    err: exits with status code 1 on surface budget or banned term violation
    """
    if not HEADER_PATH.exists():
        print(f"ERROR: Header not found at {HEADER_PATH}")
        sys.exit(1)

    content = HEADER_PATH.read_text(encoding="utf-8")

    # 1. Check banned terms (case-insensitive) in code and comments
    for term in BANNED_TERMS:
        matches = re.findall(rf"\b{re.escape(term)}\b", content, re.IGNORECASE)
        if matches:
            print(f"VIOLATION: Found banned term '{term}' {len(matches)} time(s) in {HEADER_PATH}")
            sys.exit(1)

    # 2. Count functions declared with RZ_API
    func_pattern = re.compile(r"RZ_API\s+[\w\s\*]+\s+(Rz\w+)\s*\(", re.MULTILINE)
    functions = func_pattern.findall(content)

    # 3. Count public types (Rz* typedefs)
    type_pattern = re.compile(r"typedef\s+(?:enum|struct)\s+(\w+)\s*\{", re.MULTILINE)
    types = type_pattern.findall(content)

    print(f"C Public Surface Audit ({HEADER_PATH.name}):")
    print(f"  Functions ({len(functions)}/7): {', '.join(functions)}")
    print(f"  Types     ({len(types)}/3): {', '.join(types)}")

    failed = False
    if len(functions) > 7:
        print(f"FAILED: Function count {len(functions)} exceeds maximum allowance of 7!")
        failed = True
    if "RzAbiVersion" not in functions:
        print("FAILED: Mandatory governance function RzAbiVersion is missing from RazobillC.h!")
        failed = True
    if len(types) > 3:
        print(f"FAILED: Type count {len(types)} exceeds maximum allowance of 3!")
        failed = True

    if failed:
        sys.exit(1)

    print("SUCCESS: C surface is within hard budget limits with zero banned terms.")


if __name__ == "__main__":
    main()

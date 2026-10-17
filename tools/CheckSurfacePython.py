#!/usr/bin/env python3
# =============================================================================
# surface budget enforcer: pyrazobill Python package surface and docstrings
#
# enforces hard limits: <= 8 public symbols exported in __all__, and zero
# banned scientific terms in public docstrings or module documentation
# ~ audits __all__ exports to ensure stable public Python API surface
# =============================================================================

import sys
from pathlib import Path

# Ensure lib/python is in sys.path
root_dir = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(root_dir / "lib" / "python"))

import pyrazobill as rz

MAX_NAMES = 8
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
    """audit pyrazobill __all__ export budget and docstring banned terms.

    inv: pyrazobill exports <= 8 names, all defined, with 0 banned terms
    err: exits with status code 1 on budget or banned term violation
    """
    names = rz.__all__
    print("Python Public Surface Audit (pyrazobill):")
    print(f"  Names ({len(names)}/{MAX_NAMES}): {', '.join(names)}")

    errors = []

    if len(names) > MAX_NAMES:
        errors.append(f"Exceeded hard cap of {MAX_NAMES} public names: found {len(names)}.")

    # Check module docstring
    mod_doc = (rz.__doc__ or "").lower()
    for term in BANNED_TERMS:
        if term in mod_doc:
            errors.append(f"Banned term '{term}' found in module docstring.")

    # Check each public symbol docstring
    for name in names:
        obj = getattr(rz, name, None)
        if obj is None:
            errors.append(f"Symbol '{name}' listed in __all__ but not defined.")
            continue
        obj_doc = (getattr(obj, "__doc__", "") or "").lower()
        for term in BANNED_TERMS:
            if term in obj_doc:
                errors.append(f"Banned term '{term}' found in docstring of {name}.")

    if errors:
        print("FAILURES:")
        for err in errors:
            print(f"  - {err}")
        sys.exit(1)

    print("SUCCESS: Python surface is within hard budget limits with zero banned terms.")
    sys.exit(0)


if __name__ == "__main__":
    main()

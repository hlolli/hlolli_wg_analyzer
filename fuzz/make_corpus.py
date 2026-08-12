#!/usr/bin/env python3
"""Materialize checked text seeds into raw libFuzzer corpus files."""

from __future__ import annotations

import base64
import binascii
import shutil
import sys
from pathlib import Path


def decode(source: Path) -> bytes:
    data = source.read_bytes()
    if source.suffix == ".hex":
        return binascii.unhexlify(b"".join(data.split()))
    if source.suffix == ".b64":
        return base64.b64decode(b"".join(data.split()), validate=True)
    return data


def output_name(path: Path) -> str:
    if path.suffix in {".hex", ".b64"}:
        return path.stem
    return path.name


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: make_corpus.py SOURCE_ROOT OUTPUT_ROOT", file=sys.stderr)
        return 2
    source_root = Path(sys.argv[1])
    output_root = Path(sys.argv[2])
    if not source_root.is_dir():
        print(f"missing corpus seed root: {source_root}", file=sys.stderr)
        return 1
    if output_root.exists():
        shutil.rmtree(output_root)
    output_root.mkdir(parents=True)
    for source in sorted(path for path in source_root.rglob("*") if path.is_file()):
        relative = source.relative_to(source_root)
        target = output_root / relative.parent / output_name(relative)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(decode(source))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Compile extracted production functions and run deterministic regressions."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[3]
TOOLS = Path(__file__).resolve().parent
BUILD = ROOT / "hnat-audit-build/host-tests"
REL = Path("target/linux/mediatek/files-6.6/drivers/net/ethernet/mediatek/mtk_hnat/hnat.c")


def function(text, name):
    match = re.search(r"^(?:static\s+)?[\w *]+\b" + re.escape(name) +
                      r"\([^;{]*\)\s*\{", text, re.M)
    if not match:
        raise RuntimeError(f"Missing production function: {name}")
    start = text.index("{", match.start())
    token = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', re.S)
    depth = 0
    for part in token.finditer(text, start):
        if part.group() == "{":
            depth += 1
        elif part.group() == "}":
            depth -= 1
            if depth == 0:
                return text[match.start():part.end()] + "\n"
    raise RuntimeError(f"Unbalanced function: {name}")


def run_alloc(source, baseline):
    source_file = source / REL
    if not source_file.exists():
        source_file = source / "drivers/net/ethernet/mediatek/mtk_hnat/hnat.c"
    text = source_file.read_text()
    build = BUILD / ("baseline" if baseline else "fixed")
    build.mkdir(parents=True, exist_ok=True)
    names = (["hnat_start"] if baseline else
             ["hnat_free_tables", "hnat_alloc_table", "hnat_alloc_tables",
              "hnat_start", "entry_delete_by_mac"])
    extracted = "\n".join(function(text, name) for name in names)
    (build / "alloc_extracted.h").write_text(extracted)
    print(f"SOURCE={source_file}; SHA256={hashlib.sha256(text.encode()).hexdigest()}", flush=True)
    for mode in ("v1", "rx_v2"):
        binary = build / f"test_alloc_{mode}"
        flags = ["-DTEST_RX_V2"] if mode == "rx_v2" else []
        if baseline:
            # The original unsigned >= 0 loop also triggers -Wtype-limits;
            # permit that known warning only when reproducing the old bug.
            flags += ["-DTEST_BASELINE", "-Wno-unused-function", "-Wno-unused-variable",
                      "-Wno-type-limits"]
        command = ["gcc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra",
                   "-Werror", "-Wno-sign-compare", "-fsanitize=address,undefined",
                   "-fno-omit-frame-pointer", *flags, "-I", str(build),
                   str(TOOLS / "test_alloc.c"), "-o", str(binary)]
        subprocess.run(command, check=True)
        result = subprocess.run([str(binary)], text=True, capture_output=True,
                                env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0" if baseline else "detect_leaks=1"},
                                timeout=60)
        (build / f"{mode}.log").write_text(result.stdout + result.stderr)
        print(result.stdout + result.stderr, end="", flush=True)
        if baseline:
            if result.returncode != 42 or "exceeds allocation" not in result.stderr:
                raise RuntimeError("Baseline did not reproduce the table-geometry violation")
            print(f"BASELINE_REPRODUCED={mode}", flush=True)
        else:
            result.check_returncode()
            if "runtime error:" in result.stderr or "ERROR: AddressSanitizer" in result.stderr:
                raise RuntimeError("Sanitizer detected a regression")
            print(f"ALLOC_TESTS_OK={mode}", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", action="store_true")
    parser.add_argument("--source", type=Path,
                        help="Full OpenWrt source or expanded Linux tree to test")
    parser.add_argument("--output", type=Path, default=BUILD,
                        help="Directory for generated test code, binaries and logs")
    args = parser.parse_args()
    source = args.source or ROOT / ("immortalwrt-mt798x-6.6" if args.baseline else "main_fix_hnat-source")
    BUILD = args.output.resolve()
    run_alloc(source, args.baseline)

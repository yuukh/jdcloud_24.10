#!/usr/bin/env python3
"""Run hardware-independent regressions in a separate instrumented kernel.

--kernel must be the expanded, fully patched Linux tree. --source points at
the full OpenWrt checkout containing the current driver overlay. No test code
or test configuration is copied back into either production source tree.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

from run_host_tests import function

HERE = Path(__file__).resolve().parent


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 8))
    args = parser.parse_args()
    source, kernel, output = args.source.resolve(), args.kernel.resolve(), args.output.resolve()
    if source == output or kernel == output or output in kernel.parents:
        raise RuntimeError("Test output must be separate from production sources")
    output.mkdir(exist_ok=True, parents=True)
    tree = output / "linux-test"
    stamp = output / "test-tree-ready"
    if not stamp.exists():
        if tree.exists():
            raise RuntimeError("Incomplete test tree exists; inspect it rather than overwrite")
        subprocess.run(["cp", "--reflink=auto", "-a", str(kernel), str(tree)], check=True)
        stamp.write_text(str(kernel) + "\n")
    elif stamp.read_text().strip() != str(kernel):
        raise RuntimeError("Test tree belongs to a different kernel input")

    driver_rel = "drivers/net/ethernet/mediatek/mtk_hnat"
    overlay = source / "target/linux/mediatek/files-6.6" / driver_rel
    inputs = [kernel / "drivers/net/ethernet/mediatek/mtk_eth_soc.c",
              kernel / "net/core/gro.c", overlay / "hnat.c", overlay / "hnat_cpu.h",
              HERE / "hnat_audit_test.c", HERE / "kunitconfig"]
    metadata = {str(path): digest(path) for path in inputs}
    metadata["kernel_source"] = str(kernel)
    (output / "inputs.json").write_text(json.dumps(metadata, indent=2) + "\n")

    driver = inputs[0].read_text()
    gro = inputs[1].read_text()
    extracted = "\n".join(function(driver, name) for name in
                          ("mtk_tx_set_dma_desc_v1", "mtk_tx_set_dma_desc_v2"))
    extracted += "\n".join(function(gro, name) for name in
                           ("gro_list_prepare_tc_ext", "gro_list_prepare"))
    (tree / "lib/hnat_audit_extracted.h").write_text(extracted)
    hnat = inputs[2].read_text()
    roaming = "\n".join(function(hnat, name) for name in
                        ("hnat_roaming_notify", "hnat_roam_handler",
                         "hnat_roaming_enable", "hnat_roaming_disable"))
    (tree / "lib/hnat_roam_extracted.h").write_text(roaming)
    shutil.copyfile(overlay / "hnat_cpu.h", tree / driver_rel / "hnat_cpu.h")
    shutil.copyfile(HERE / "hnat_audit_test.c", tree / "lib/hnat_audit_test.c")
    makefile = tree / "lib/Makefile"
    text = makefile.read_text()
    line = "obj-$(CONFIG_KUNIT) += hnat_audit_test.o\n"
    if line not in text:
        makefile.write_text(text + "\n" + line)

    build = output / "build"
    log = output / "kunit.log"
    command = [sys.executable, str(tree / "tools/testing/kunit/kunit.py"), "run",
               "--arch=arm64", f"--jobs={args.jobs}", f"--build_dir={build}",
               f"--kunitconfig={HERE / 'kunitconfig'}", "--timeout=180",
               "--make_options=CC=ccache gcc", "hnat-audit*"]
    print(f"KUNIT_INPUTS={output / 'inputs.json'}", flush=True)
    with log.open("w") as stream:
        subprocess.run(command, cwd=tree, stdout=stream, stderr=subprocess.STDOUT, check=True)
    raw = (build / "test.log").read_text()
    if re.search(r"KASAN:|UBSAN:|runtime error:|BUG:|WARNING:|possible circular locking|inconsistent lock state", raw):
        raise RuntimeError(f"Kernel diagnostics require review: {build / 'test.log'}")
    expected = len(re.findall(r"\bKUNIT_CASE\(", (HERE / "hnat_audit_test.c").read_text()))
    totals = re.search(r"# Totals: pass:(\d+) fail:(\d+) skip:(\d+) total:(\d+)", raw)
    if not totals or tuple(map(int, totals.groups())) != (expected, 0, 0, expected):
        raise RuntimeError("KUnit did not execute and pass every expected regression")
    print(f"KUNIT_OK={expected}; LOG={log}", flush=True)


if __name__ == "__main__":
    os.environ["LC_ALL"] = "C"
    main()

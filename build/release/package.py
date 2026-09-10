#!/usr/bin/env python3
"""Record exact source/binary provenance and include dependency licenses."""
import gzip
import hashlib
import json
from pathlib import Path
import subprocess
import sys

stage, external = map(Path, sys.argv[1:3])
revision, arch = sys.argv[3:5]
licenses = subprocess.check_output(
    ["find", "-L", str(external), "(", "-name", "LICENSE", "-o", "-name", "COPYING", ")", "-type", "f"],
    text=True,
).splitlines()
if len(licenses) < 10:
    raise RuntimeError("Dependency licenses missing from Bazel output")
with gzip.open(stage / "licenses.txt.gz", "wt") as output:
    for name in sorted(licenses):
        path = Path(name)
        output.write(f"\n--- {path.relative_to(external)} ---\n")
        output.write(path.read_text(errors="replace"))
info = {
    "repository": "indie-technologies/cloud-spanner-emulator",
    "revision": revision,
    "platform": f"linux/{arch}",
    "configuration": "opt",
    "binaries_sha256": {name: hashlib.sha256((stage / name).read_bytes()).hexdigest()
                        for name in ("emulator_main", "gateway_main")},
}
(stage / "build-info.json").write_text(json.dumps(info, indent=2) + "\n")

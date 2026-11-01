#!/usr/bin/env python3
"""Set staged and installed RUNPATHs without modifying hardlinked SDK inputs."""
import os
from pathlib import Path
import shutil
import subprocess
import sys

dist = Path(sys.argv[1]).resolve()
directories = [
    dist,
    dist / "runtime/deployment/lib",
    dist / "runtime/xrt/lib",
    dist / "runtime/compat",
]
installed = [
    "/usr/lib/biopass",
    "/opt/ryzen-ai/deployment/lib",
    "/opt/ryzen-ai/xrt/lib",
    "/opt/ryzen-ai/compat",
]
changed = 0
for path in sorted(dist.rglob("*")):
    if path.is_symlink() or not path.is_file():
        continue
    with path.open("rb") as stream:
        if stream.read(4) != b"\x7fELF":
            continue
    paths = ["$ORIGIN"] + [
        "$ORIGIN/" + os.path.relpath(directory, path.parent)
        for directory in directories if directory != path.parent
    ] + installed
    desired = ":".join(dict.fromkeys(paths))
    result = subprocess.run(
        ["patchelf", "--print-rpath", str(path)], capture_output=True, text=True
    )
    if result.returncode or result.stdout.strip() == desired:
        continue
    temporary = path.with_name(path.name + ".runpath-new")
    shutil.copy2(path, temporary)
    subprocess.run(["patchelf", "--set-rpath", desired, str(temporary)], check=True)
    temporary.replace(path)
    changed += 1
print(f"Updated RUNPATH on {changed} ELF files")

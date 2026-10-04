#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Fetch the exact edk2 revision and submodules needed by the Linux emulator."""
import argparse
import json
import pathlib
import subprocess

HERE = pathlib.Path(__file__).resolve().parent
LOCK = json.loads((HERE / "lock.json").read_text())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("workspace", type=pathlib.Path, help="directory for an isolated edk2 checkout")
    args = parser.parse_args()
    edk2 = args.workspace.resolve() / "edk2"
    if not edk2.exists():
        edk2.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "clone", "--depth", "1", "--branch", LOCK["edk2_tag"],
                        LOCK["edk2_repository"], str(edk2)], check=True)
    actual = subprocess.check_output(["git", "-C", str(edk2), "rev-parse", "HEAD"], text=True).strip()
    if actual != LOCK["edk2_commit"]:
        parser.error(f"{edk2} is at {actual}; expected {LOCK['edk2_commit']}. Use a new workspace.")
    changed = subprocess.check_output(["git", "-C", str(edk2), "status", "--porcelain",
                                       "--untracked-files=no", "--ignore-submodules=none"], text=True)
    if changed:
        parser.error(f"{edk2} has tracked changes or dirty submodules. Use a new workspace.")
    subprocess.run(["git", "-C", str(edk2), "submodule", "update", "--init", "--depth", "1",
                    "--", *LOCK["submodules"]], check=True)
    print(f"Pinned edk2 checkout ready: {edk2}")


if __name__ == "__main__":
    main()

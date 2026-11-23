#!/usr/bin/env python3
"""Check that our RTI DLL exports every symbol a federate imports from its RTI.

Windows, from a "Developer Command Prompt for VS 2019" (dumpbin on PATH):

    python tools\\check_imports.py path\\to\\federate.exe build\\bin\\Release\\rti1516e.dll

Optional: --rti-dll-name name.dll  if the federate imports a differently named
DLL (default: same file name as the DLL given).

Prints the decorated (and undecorated) names missing from our exports.
Exit code 0 = drop-in replacement possible.
"""
import argparse
import os
import re
import subprocess
import sys


def dumpbin(*args):
    return subprocess.run(["dumpbin", *args], capture_output=True, text=True, check=True).stdout


def imports_from(exe, dll_name):
    out = dumpbin("/imports", exe)
    symbols, inside = set(), False
    for line in out.splitlines():
        s = line.strip()
        if s.lower() == dll_name.lower():
            inside = True
            continue
        if inside:
            if s.lower().endswith(".dll"):
                break
            m = re.match(r"^[0-9A-F]+\s+(\S+)$", s)
            if m:
                symbols.add(m.group(1))
    return symbols


def exports_of(dll):
    out = dumpbin("/exports", dll)
    symbols = set()
    for line in out.splitlines():
        m = re.match(r"^\s*\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+)", line)
        if m:
            symbols.add(m.group(1))
    return symbols


def undecorate(name):
    try:
        return subprocess.run(["undname", name], capture_output=True, text=True).stdout.splitlines()[-1]
    except Exception:
        return ""


def main():
    p = argparse.ArgumentParser()
    p.add_argument("federate")
    p.add_argument("dll")
    p.add_argument("--rti-dll-name")
    a = p.parse_args()
    dll_name = a.rti_dll_name or os.path.basename(a.dll)

    needed = imports_from(a.federate, dll_name)
    if not needed:
        print(f"{a.federate} imports nothing from {dll_name}: check the name with dumpbin /dependents")
        return 2
    provided = exports_of(a.dll)
    missing = sorted(needed - provided)
    print(f"{len(needed)} symbols imported from {dll_name}, {len(missing)} missing")
    for name in missing:
        print(f"  {name}\n      {undecorate(name)}")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Make refs.inc from real CPUs' serial logs.

  refs.py LOG [LOG...] > refs.inc

From each log it takes the first complete pass after the last CPUTEST
banner: one line per group, keyed by the CPU's vendor and signature, with
the group's definition hash (def=), test count and CRCs. Groups that had
mismatches are left out; skipped groups are kept as skipped. Lines already
in refs.inc for other CPUs are kept (pass --keep refs.inc)."""

import re
import sys


def parse(path):
    lines = open(path, errors="replace").read().splitlines()
    starts = [i for i, l in enumerate(lines) if l.startswith("CPUTEST ")]
    if not starts:
        sys.exit("%s: no CPUTEST banner" % path)
    lines = lines[starts[-1]:]
    m = re.search(r"cpu=(\S+)(?: sig=([0-9a-f]+))?", lines[0])
    vendor, sig = m.group(1), int(m.group(2) or "0", 16)
    out = {}
    done = False
    for l in lines[1:]:
        if l.startswith("DONE "):
            done = True
            break
        g = re.match(r"GROUP (\S+) skipped:", l)
        if g:
            out[g.group(1)] = (1, 0, 0, 0, 0)
            continue
        g = re.match(r"GROUP (\S+) tests=(\d+) mismatches=(\d+) .*crc_defined=([0-9a-f]+) crc_raw=([0-9a-f]+) def=([0-9a-f]+)", l)
        if g and g.group(3) == "0":
            out[g.group(1)] = (0, int(g.group(2)), int(g.group(6), 16), int(g.group(4), 16), int(g.group(5), 16))
        elif l.startswith("GROUP ") and "def=" not in l and "skipped" not in l:
            sys.exit("%s: GROUP lines without def=: an image older than the references" % path)
    if not done:
        sys.stderr.write("%s: no DONE line: the pass is incomplete, taking what is there\n" % path)
    return vendor, sig, out


def main():
    args = sys.argv[1:]
    keep = []
    if "--keep" in args:
        i = args.index("--keep")
        keep = [l for l in open(args[i + 1]) if l.startswith("    {")]
        del args[i:i + 2]
    rows = {}
    for l in keep:
        m = re.match(r'\s*\{ "([^"]+)", 0x([0-9a-f]+), "([^"]+)"', l)
        rows[(m.group(1), int(m.group(2), 16), m.group(3))] = l.rstrip("\n")
    for path in args:
        vendor, sig, groups = parse(path)
        for name, (skipped, tests, d, cd, cr) in groups.items():
            rows[(vendor, sig, name)] = '    { "%s", 0x%04x, "%s", %d, %d, 0x%08x, 0x%08x, 0x%08x },' % (
                vendor, sig, name, skipped, tests, d, cd, cr)
    print("/* Results of real CPUs, one line per group, made by refs.py from their")
    print("   serial logs. Included into harness.c. */")
    for k in sorted(rows):
        print(rows[k])


if __name__ == "__main__":
    main()

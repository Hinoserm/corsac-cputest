#!/usr/bin/env python3
"""Compare two --dump outputs (host build vs a ROM run) line by line and
summarise where the defined results differ.

  diffdump.py REFERENCE.txt RUN.txt
"""
import collections
import sys

FLAGS = [(0x0001, "CF"), (0x0004, "PF"), (0x0010, "AF"), (0x0040, "ZF"), (0x0080, "SF"),
         (0x0400, "DF"), (0x0800, "OF")]


def lines(path):
    out = []
    for line in open(path, errors="replace"):
        line = line.strip()
        if line.startswith("D "):
            out.append(line)
    return out


def parse(line):
    # D group bytes producer+boundary in=fl,ecx,edx fl=XXXX eax ecx edx ebx ebp esi edi m=XXXXXXXX
    f = line.split()
    return dict(group=f[1], bytes=f[2], ctx=f[3], inp=f[4], flags=int(f[5][3:], 16),
                regs=[int(x, 16) for x in f[6:13]], mem=f[13])


def main():
    ref, run = lines(sys.argv[1]), lines(sys.argv[2])
    print("reference %d lines, run %d lines" % (len(ref), len(run)))
    by = collections.Counter()
    fields = collections.Counter()
    shown = 0
    for a, b in zip(ref, run):
        if a == b:
            continue
        pa, pb = parse(a), parse(b)
        if (pa["group"], pa["bytes"], pa["ctx"], pa["inp"]) != (pb["group"], pb["bytes"], pb["ctx"], pb["inp"]):
            print("streams out of step:\n  %s\n  %s" % (a, b))
            break
        what = []
        x = pa["flags"] ^ pb["flags"]
        what += [n for bit, n in FLAGS if x & bit]
        what += ["eax ecx edx ebx ebp esi edi".split()[i] for i in range(7) if pa["regs"][i] != pb["regs"][i]]
        if pa["mem"] != pb["mem"]:
            what.append("mem")
        key = (pa["group"], pa["bytes"], pa["ctx"][:-1], " ".join(what))
        by[key] += 1
        fields[" ".join(what)] += 1
        if shown < 12:
            print("ref %s\nrun %s\n" % (a, b))
            shown += 1
    print("differing lines: %d" % sum(by.values()))
    for (g, byt, ctx, what), n in sorted(by.items(), key=lambda kv: -kv[1])[:60]:
        print("%6d  %-10s %-14s after %-6s  %s" % (n, g, byt, ctx, what))
    print("by field:")
    for what, n in fields.most_common():
        print("%6d  %s" % (n, what))


if __name__ == "__main__":
    main()

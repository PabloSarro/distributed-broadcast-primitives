#!/usr/bin/env python3
"""Validate perfect-links logs written by stress.py (receiver = process 1).

usage: validate_pl.py LOGSDIR [--crashed ID ...]
Crashed ids default to the `Sending SIGTERM to process N` lines of LOGSDIR/stress.log.
"""
import argparse, glob, os, re, sys

MAX_BYTES = 64 << 20
B_RE = re.compile(rb"b (\d+)\n")
D_RE = re.compile(rb"d (\d+) (\d+)\n")
RECEIVER = 1

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logs")
    ap.add_argument("--crashed", type=int, nargs="*")
    ap.add_argument("--ignore-size", action="store_true", help="do not fail on the 64 MiB cap (checks the rest)")
    args = ap.parse_args()

    crashed = set(args.crashed or [])
    stress = os.path.join(args.logs, "stress.log")
    if args.crashed is None and os.path.exists(stress):
        crashed = {int(x) for x in re.findall(r"Sending SIGTERM to process (\d+)", open(stress).read())}

    cfg = open(os.path.join(args.logs, "config")).read().split()
    m = int(cfg[0])
    errors = []
    bs, ds = {}, []  # sender -> [seq] of b lines; receiver d lines as (sender, seq)

    for path in sorted(glob.glob(os.path.join(args.logs, "proc*.output"))):
        pid = int(re.search(r"proc(\d+)\.output", path).group(1))
        data = open(path, "rb").read()
        if len(data) > MAX_BYTES and not args.ignore_size:
            errors.append(f"proc {pid}: output > 64 MiB")
        if data and not data.endswith(b"\n"):
            errors.append(f"proc {pid}: last line not newline-terminated")
        rx = D_RE if pid == RECEIVER else B_RE
        vals = []
        for n, line in enumerate(data.splitlines(keepends=True), 1):
            mt = rx.fullmatch(line)
            if not mt:
                errors.append(f"proc {pid} line {n}: bad line {line[:40]!r}")
                break
            vals.append(tuple(map(int, mt.groups())) if pid == RECEIVER else int(mt.group(1)))
        if pid == RECEIVER:
            ds = vals
        else:
            bs[pid] = vals

    for pid, seqs in bs.items():
        if seqs != list(range(1, len(seqs) + 1)):
            errors.append(f"proc {pid}: b lines are not exactly 1..k")
        if pid not in crashed and len(seqs) != m:
            errors.append(f"proc {pid}: correct sender logged {len(seqs)} of {m} b lines")

    if len(ds) != len(set(ds)):
        errors.append("receiver: duplicate d lines (PL2)")
    delivered = {}
    for s, q in ds:
        delivered.setdefault(s, set()).add(q)
        if s not in bs or q < 1 or q > len(bs[s]):
            errors.append(f"receiver: d {s} {q} has no matching b (PL3)")
            break

    if RECEIVER not in crashed:
        for s, seqs in bs.items():
            if s not in crashed and len(delivered.get(s, ())) != len(seqs):
                errors.append(f"PL1: sender {s} sent {len(seqs)}, receiver delivered {len(delivered.get(s, ()))}")

    total = len(ds)
    print(f"crashed={sorted(crashed)} delivered={total} senders={len(bs)}")
    for e in errors[:20]:
        print("FAIL:", e)
    print("FAILED" if errors else "OK")
    sys.exit(1 if errors else 0)

main()

#!/usr/bin/env python3
"""Run one command with bounded wall time and child CPU accounting."""

import os
import resource
import signal
import subprocess
import sys
import time


def usage() -> None:
    print("usage: fma-measure.py OUTPUT TIMEOUT_SECONDS COMMAND [ARG ...]", file=sys.stderr)


if len(sys.argv) < 4:
    usage()
    raise SystemExit(2)

output = sys.argv[1]
try:
    timeout = float(sys.argv[2])
except ValueError:
    usage()
    raise SystemExit(2)
command = sys.argv[3:]

before = resource.getrusage(resource.RUSAGE_CHILDREN)
started = time.monotonic()
process = subprocess.Popen(command, start_new_session=True)
timed_out = False
try:
    status = process.wait(timeout=timeout)
except subprocess.TimeoutExpired:
    timed_out = True
    os.killpg(process.pid, signal.SIGTERM)
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait()
    status = 124
finished = time.monotonic()
after = resource.getrusage(resource.RUSAGE_CHILDREN)

with open(output, "w", encoding="utf-8") as stream:
    stream.write(f"wall_s={finished - started:.6f}\n")
    stream.write(f"user_s={after.ru_utime - before.ru_utime:.6f}\n")
    stream.write(f"system_s={after.ru_stime - before.ru_stime:.6f}\n")
    stream.write(f"maxrss_kib={after.ru_maxrss}\n")
    stream.write(f"timed_out={int(timed_out)}\n")

raise SystemExit(status if 0 <= status <= 255 else 1)

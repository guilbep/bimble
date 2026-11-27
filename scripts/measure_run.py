#!/usr/bin/env python3
"""Run one command under limits and print what GNU `time -v` prints (this host has no /usr/bin/time).

  python3 -I measure_run.py --vmem-gb 9 --timeout-s 1200 --out res.json -- <command...>

- run it inside a cgroup scope with a memory cap (systemd-run --user --scope -p MemoryMax=8G
  -p MemorySwapMax=0 -p OOMPolicy=continue ...): it reports the scope's memory.max, memory.peak
  (this wrapper, a few MB, included) and the oom_kill count of memory.events during the run
- optional address-space limit (--vmem-gb, RLIMIT_AS = `ulimit -v`); not used for the final runs:
  address space is not RAM, Blender and the mesher reserve far more than they touch
- wall-clock limit: the whole process group is killed after --timeout-s
- wall time and peak RSS from wait4() rusage, the same source as GNU time: the peak RSS is the
  largest single process of the tree (children that were waited for), not their sum.
"""

import argparse
import json
import os
import resource
import signal
import subprocess
import sys
import time


def mem_available_kb():
    with open("/proc/meminfo") as fh:
        for line in fh:
            if line.startswith("MemAvailable:"):
                return int(line.split()[1])
    return 0


def cgroup_dir():
    try:
        with open("/proc/self/cgroup") as fh:
            return "/sys/fs/cgroup" + fh.read().strip().split(":", 2)[2]
    except Exception:
        return None


def read(cg, name):
    try:
        with open(os.path.join(cg, name)) as fh:
            return fh.read().strip()
    except Exception:
        return None


def oom_kills(cg):
    t = read(cg, "memory.events")
    if t is None:
        return None
    for line in t.splitlines():
        k, v = line.split()
        if k == "oom_kill":
            return int(v)
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vmem-gb", type=float, default=0.0,
                    help="address-space limit: upper bound, lowered to MemAvailable - 1.5 GB; 0 = none (use a cgroup cap)")
    ap.add_argument("--timeout-s", type=float, default=1200)
    ap.add_argument("--out")
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    cmd = a.cmd[1:] if a.cmd and a.cmd[0] == "--" else a.cmd

    avail_gb = mem_available_kb() / 1024 / 1024
    limit_gb = min(a.vmem_gb, avail_gb - 1.5) if a.vmem_gb > 0 else None
    cg = cgroup_dir()
    cg_max = read(cg, "memory.max")
    oom0 = oom_kills(cg)

    def pre():
        os.setsid()
        if limit_gb is not None:
            limit = int(limit_gb * 1024**3)
            resource.setrlimit(resource.RLIMIT_AS, (limit, limit))

    t0 = time.perf_counter()
    p = subprocess.Popen(cmd, preexec_fn=pre)
    timed_out = False
    while True:
        pid, status, ru = os.wait4(p.pid, os.WNOHANG)
        if pid:
            break
        if time.perf_counter() - t0 > a.timeout_s:
            timed_out = True
            os.killpg(p.pid, signal.SIGKILL)
            pid, status, ru = os.wait4(p.pid, 0)
            break
        time.sleep(0.05)
    wall = time.perf_counter() - t0
    res = {
        "cmd": cmd,
        "mem_available_gb_at_start": round(avail_gb, 2),
        "vmem_limit_gb": None if limit_gb is None else round(limit_gb, 2),
        "cgroup": cg,
        "cgroup_memory_max": cg_max,
        "cgroup_memory_peak_mb": None if read(cg, "memory.peak") is None else round(int(read(cg, "memory.peak")) / 2**20, 1),
        "cgroup_oom_kills": None if oom0 is None else oom_kills(cg) - oom0,
        "wall_s": round(wall, 3),
        "user_s": round(ru.ru_utime, 3),
        "sys_s": round(ru.ru_stime, 3),
        "max_rss_mb": round(ru.ru_maxrss / 1024, 1),
        "exit_code": os.waitstatus_to_exitcode(status),
        "timed_out": timed_out,
    }
    print("MEASURE " + json.dumps(res), file=sys.stderr, flush=True)
    if a.out:
        with open(a.out, "w") as fh:
            json.dump(res, fh, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())

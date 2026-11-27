#!/usr/bin/env python3
"""Load test (#43) and performance bench (#42) over several hla-load processes.

  hla_load.py run   --exe hla-load [--federates N] [--total T --first I] [hla-load options]
  hla_load.py bench --exe hla-load [--quick] [--markdown FILE] [--json FILE]

run    starts N hla-load processes (one federate each) on this machine and
       checks their results. On several machines, run it on each with the same
       --federation and --total (all the federates), and --first set to the
       index of its first federate: hosts A and B with 4 federates each,
         A: hla_load.py run --exe ... --federation Big --total 8 --first 0 --federates 4
         B: hla_load.py run --exe ... --federation Big --total 8 --first 4 --federates 4
       Index 0 drives the synchronization points: start it on any host.
bench  runs a series of load and pingpong measurements on this machine and
       prints them as a Markdown table (docs/performance.md).

The broker comes from --settings, else HLA_MQTT_BROKER (hla-load default:
mqtt://localhost:1883). Exit code 0 when every federate succeeded.
"""

import argparse
import json
import os
import platform
import subprocess
import sys
import time
import uuid

PASS_THROUGH = ["settings", "fom", "mode", "objects", "attributes", "updates", "payload", "count", "timeout"]


def run_federates(exe, federation, total, first, local, opts, quiet=False):
    """Starts `local` processes (indexes first..first+local-1); returns their RESULT dicts."""
    procs = []
    for i in range(first, first + local):
        cmd = [exe, "--index", str(i), "--federates", str(total), "--federation", federation]
        for k in PASS_THROUGH:
            v = opts.get(k)
            if v is not None:
                cmd += ["--" + k, str(v)]
        procs.append((i, subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)))
    results = []
    for i, p in procs:
        out, err = p.communicate()
        result = None
        for line in out.splitlines():
            if line.startswith("RESULT "):
                result = json.loads(line[len("RESULT "):])
        if result is None:
            result = {"index": i, "ok": False}
        result["exitCode"] = p.returncode
        if not quiet or p.returncode != 0:
            for line in err.splitlines():
                print(line, file=sys.stderr)
        results.append(result)
    return results


def summarize(results):
    ok = all(r.get("ok") and r["exitCode"] == 0 for r in results)
    s = {"ok": ok, "federates": len(results), "mode": results[0].get("mode") if results else None}
    if s["mode"] == "load":
        received = sum(r.get("received", 0) for r in results)
        window = max((r.get("receiveSeconds", 0) for r in results), default=0)
        s.update({
            "received": received,
            "expected": sum(r.get("expected", 0) for r in results),
            "seconds": window,
            "receivedPerSecond": received / window if window > 0 else 0.0,
            "latencyMsP50": max((r.get("latencyMsP50", 0) for r in results), default=0),
            "latencyMsP99": max((r.get("latencyMsP99", 0) for r in results), default=0),
        })
    elif s["mode"] == "pingpong":
        pinger = next((r for r in results if r.get("index") == 0), {})
        s.update({k: pinger.get(k) for k in ("count", "rttMsMean", "rttMsP50", "rttMsP99", "rttMsMin", "rttMsMax")})
    return s


def cmd_run(a):
    total = a.total or a.federates
    opts = {k: getattr(a, k) for k in PASS_THROUGH}
    start = time.time()
    results = run_federates(a.exe, a.federation, total, a.first, a.federates, opts)
    s = summarize(results)
    s["wallSeconds"] = round(time.time() - start, 3)
    for r in results:
        print("federate %s: %s" % (r.get("index"), json.dumps(r)))
    print("SUMMARY " + json.dumps(s))
    return 0 if s["ok"] else 1


def bench_case(exe, settings, n, **opts):
    federation = "HlaBench-" + uuid.uuid4().hex[:8]
    opts["settings"] = settings
    results = run_federates(exe, federation, n, 0, n, opts, quiet=True)
    s = summarize(results)
    s.update({"n": n, **{k: v for k, v in opts.items() if k != "settings"}})
    print(json.dumps(s), file=sys.stderr)
    return s


def cmd_bench(a):
    q = a.quick
    load = []
    # Scaling with the number of federates (every federate updates its objects).
    for n in ([2, 4] if q else [2, 4, 8, 16]):
        load.append(bench_case(a.exe, a.settings, n, mode="load", objects=10, attributes=4,
                               updates=50 if q else 200, payload=64))
    # Scaling with the number of objects and attributes (2 federates).
    for objects, attributes in ([(100, 4), (10, 32)] if q else [(100, 4), (1000, 4), (10, 16), (10, 32)]):
        load.append(bench_case(a.exe, a.settings, 2, mode="load", objects=objects, attributes=attributes,
                               updates=20 if q else 50, payload=64))
    # Payload size.
    for payload in ([1024] if q else [1024, 16384]):
        load.append(bench_case(a.exe, a.settings, 2, mode="load", objects=10, attributes=1,
                               updates=50 if q else 200, payload=payload))
    ping = []
    for n, payload in ([(2, 64), (4, 64)] if q else [(2, 64), (2, 4096), (4, 64), (8, 64)]):
        ping.append(bench_case(a.exe, a.settings, n, mode="pingpong", count=200 if q else 1000, payload=payload))

    lines = []
    lines.append("Platform: %s, %s, %d logical processors; broker: %s" % (
        platform.platform(), platform.processor() or platform.machine(), os.cpu_count() or 0,
        a.settings or os.environ.get("HLA_MQTT_BROKER", "mqtt://localhost:1883")))
    lines.append("")
    lines.append("| Federates | Objects/fed. | Attributes | Payload (B) | Updates/object | Reflects received | Reflects/s | Latency p50 (ms) | Latency p99 (ms) | OK |")
    lines.append("|---:|---:|---:|---:|---:|---:|---:|---:|---:|:--:|")
    for s in load:
        lines.append("| %d | %d | %d | %d | %d | %d | %.0f | %.2f | %.2f | %s |" % (
            s["n"], s["objects"], s["attributes"], s["payload"], s["updates"], s.get("received", 0),
            s.get("receivedPerSecond", 0), s.get("latencyMsP50", 0), s.get("latencyMsP99", 0),
            "yes" if s["ok"] else "**no**"))
    lines.append("")
    lines.append("| Federates | Payload (B) | Round trips | Mean (ms) | p50 (ms) | p99 (ms) | Min (ms) | Max (ms) | OK |")
    lines.append("|---:|---:|---:|---:|---:|---:|---:|---:|:--:|")
    for s in ping:
        lines.append("| %d | %d | %s | %.3f | %.3f | %.3f | %.3f | %.3f | %s |" % (
            s["n"], s["payload"], s.get("count"), s.get("rttMsMean") or 0, s.get("rttMsP50") or 0,
            s.get("rttMsP99") or 0, s.get("rttMsMin") or 0, s.get("rttMsMax") or 0,
            "yes" if s["ok"] else "**no**"))
    table = "\n".join(lines)
    print(table)
    if a.markdown:
        with open(a.markdown, "w", encoding="utf-8", newline="\n") as f:
            f.write(table + "\n")
    if a.json:
        with open(a.json, "w", encoding="utf-8", newline="\n") as f:
            json.dump({"load": load, "pingpong": ping}, f, indent=2)
    return 0 if all(s["ok"] for s in load + ping) else 1


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="command", required=True)

    r = sub.add_parser("run", help="start federates on this machine and check them")
    r.add_argument("--exe", required=True, help="path of hla-load")
    r.add_argument("--federation", default="HlaLoad-" + uuid.uuid4().hex[:8])
    r.add_argument("--federates", type=int, default=4, help="processes started here")
    r.add_argument("--total", type=int, default=0, help="federates on all the machines (default: --federates)")
    r.add_argument("--first", type=int, default=0, help="index of the first federate started here")
    for k in PASS_THROUGH:
        r.add_argument("--" + k)
    r.set_defaults(func=cmd_run)

    b = sub.add_parser("bench", help="performance series on this machine")
    b.add_argument("--exe", required=True)
    b.add_argument("--settings")
    b.add_argument("--quick", action="store_true", help="smaller series (about a minute)")
    b.add_argument("--markdown", help="write the tables to this file")
    b.add_argument("--json", help="write the raw results to this file")
    b.set_defaults(func=cmd_bench)

    a = p.parse_args()
    sys.exit(a.func(a))


if __name__ == "__main__":
    main()

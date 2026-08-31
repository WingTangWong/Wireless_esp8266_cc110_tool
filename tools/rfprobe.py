#!/usr/bin/env python3
"""rfprobe - drive the CC1101 318 MHz tool's HTTP API from a host.

Stdlib only. Point it at a flashed, networked D1 Mini:

    tools/rfprobe.py --host cc1101.local status
    tools/rfprobe.py --host 192.168.4.1 selftest
    tools/rfprobe.py tune 318.0 650
    tools/rfprobe.py sweep 317.7 318.3 --step 20 --json | jq .summary
    tools/rfprobe.py record 2000 --decode
    tools/rfprobe.py sample play inner_gate --repeat 3
    tools/rfprobe.py watch --interval 1
    tools/rfprobe.py gate

Default host is $CC1101_HOST or cc1101.local. `--json` prints the raw
response object for piping to jq; otherwise output is a short summary.
`--timeout` sets the per-request HTTP timeout.
"""

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

DEFAULT_HOST = os.environ.get("CC1101_HOST", "cc1101.local")


class ApiError(RuntimeError):
    pass


def call(host, path, params=None, method="GET", timeout=10.0):
    url = f"http://{host}{path}"
    if params:
        url += "?" + urllib.parse.urlencode(params)
    req = urllib.request.Request(url, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", "replace")
        try:
            data = json.loads(body)
        except ValueError:
            raise ApiError(f"HTTP {e.code} from {path}: {body[:200]}")
        raise ApiError(f"HTTP {e.code} from {path}: {data.get('error', body[:200])}")
    except urllib.error.URLError as e:
        raise ApiError(f"cannot reach {url}: {e.reason}")
    try:
        data = json.loads(body)
    except ValueError:
        raise ApiError(f"non-JSON response from {path}: {body[:200]}")
    if isinstance(data, dict) and data.get("ok") is False:
        raise ApiError(f"{path}: {data.get('error', 'ok=false')}")
    return data


def api(args, path, params=None, method="GET"):
    return call(args.host, path, params, method, timeout=args.timeout)


def emit(args, data, summary):
    if args.json:
        json.dump(data, sys.stdout, indent=2)
        sys.stdout.write("\n")
    else:
        print(summary)


def cmd_status(args):
    d = api(args, "/api/status")
    emit(args, d, (
        f"fw {d.get('firmwareVersion')} ({d.get('firmwareBuild')})\n"
        f"radio: {'OK' if d.get('radio') else 'NOT DETECTED'} "
        f"partnum={d.get('radioPartnum')} version={d.get('radioVersion')}\n"
        f"mode: {d.get('mode')}  busy={d.get('busy')}  heap={d.get('heap')}  "
        f"fs={d.get('fsUsedBytes')}/{d.get('fsTotalBytes')}\n"
        f"target: {d.get('frequencyHz', 0) / 1e6:.6f} MHz  bw={d.get('bandwidthKhz')} kHz\n"
        f"wifi: {'up' if d.get('wifiConnected') else 'down'} {d.get('wifiIp')} "
        f"({d.get('wifiSsid')} {d.get('wifiRssi')} dBm)  "
        f"ap: {d.get('apSsid')} {d.get('apIp')} clients={d.get('apClients')}"
    ))


def cmd_selftest(args):
    d = api(args, "/api/selftest")
    checks = d.get("checks", {})
    lines = [f"selftest: {'PASS' if d.get('ok') else 'FAIL'}  fw {d.get('firmwareVersion')}"]
    for k, v in checks.items():
        lines.append(f"  {'ok ' if v else 'FAIL'} {k}")
    lines.append(
        f"  partnum={d.get('radioPartnum')} version={d.get('radioVersion')} "
        f"heap={d.get('heap')} fs={d.get('fsUsedBytes')}/{d.get('fsTotalBytes')}"
    )
    emit(args, d, "\n".join(lines))
    if not d.get("ok"):
        sys.exit(1)


def cmd_tune(args):
    hz = round(args.mhz * 1e6)
    d = api(args, "/api/tune", {"hz": hz, "bw": args.bw})
    emit(args, d, f"tuned to {d.get('frequencyHz', 0) / 1e6:.6f} MHz")


def _wait_idle(host, wait, http_timeout=10.0):
    deadline = time.time() + wait
    while time.time() < deadline:
        s = call(host, "/api/status", timeout=http_timeout)
        if not s.get("busy"):
            return s
        time.sleep(0.25)
    raise ApiError("timed out waiting for device to become idle")


def cmd_sweep(args):
    start = round(args.start * 1e6)
    stop = round(args.stop * 1e6)
    step = round(args.step * 1e3)
    api(args, "/api/sweep/start",
         {"start": start, "stop": stop, "step": step, "dwell": args.dwell, "bw": args.bw})
    _wait_idle(args.host, args.wait, args.timeout)
    d = api(args, "/api/sweep/data")
    summ = d.get("summary") or {}
    emit(args, d, (
        f"{d.get('count')} points  "
        f"min={summ.get('rssiMinDbm')} max={summ.get('rssiMaxDbm')} "
        f"mean={summ.get('rssiMeanDbm')} dBm  "
        f"peak={summ.get('peakHz', 0) / 1e6:.6f} MHz"
    ))


def cmd_record(args):
    api(args, "/api/capture/start", {"ms": args.ms})
    _wait_idle(args.host, (args.ms / 1000.0) + args.wait, args.timeout)
    st = api(args, "/api/status")
    out = {"status": st}
    summary = [f"captured {st.get('pulses')} pulses in {st.get('captureDurationUs', 0) / 1000:.2f} ms"]
    if args.histogram or args.decode:
        out["histogram"] = api(args, "/api/capture/histogram")
        summary.append(f"histogram binWidthUs={out['histogram'].get('binWidthUs')}")
    if args.decode:
        out["decode"] = api(args, "/api/decode/current")
        dec = out["decode"]
        summary.append(
            f"encoding: {dec.get('encoding')}  "
            f"short/long={dec.get('short_us')}/{dec.get('long_us')} us  "
            f"protocol={dec.get('protocol_candidate')}"
        )
    emit(args, out, "\n".join(summary))


def cmd_histogram(args):
    d = api(args, "/api/capture/histogram")
    emit(args, d, f"binWidthUs={d.get('binWidthUs')} bins={len(d.get('counts', []))}")


def cmd_pulses(args):
    d = api(args, "/api/capture/pulses")
    emit(args, d, (
        f"{d.get('count')} pulses  {d.get('durationUs', 0) / 1000:.2f} ms  "
        f"@ {d.get('frequencyHz', 0) / 1e6:.6f} MHz"
    ))


def cmd_decode(args):
    d = api(args, "/api/decode/current")
    emit(args, d, (
        f"encoding: {d.get('encoding')}\n"
        f"short/long: {d.get('short_us')}/{d.get('long_us')} us  ratio {d.get('ratio')}\n"
        f"protocol: {d.get('protocol_candidate')}  {d.get('protocol_details', '')}\n"
        f"bits: {d.get('candidate_bits')}"
    ))


def cmd_samples(args):
    d = api(args, "/api/samples")
    rows = d.get("samples", [])
    lines = [f"{len(rows)} sample(s)"]
    for s in rows:
        lines.append(
            f"  {s['name']:<24} {s['frequencyHz'] / 1e6:.6f} MHz  "
            f"{s['pulseCount']} pulses  {s['durationUs'] / 1000:.1f} ms"
        )
    emit(args, d, "\n".join(lines))


def cmd_gate(args):
    d = api(args, "/api/gate")
    lines = [f"forced TX power: +{d.get('txPowerForcedDbm')} dBm  min repeats: {d.get('minRepeats')}"]
    for which in ("inner", "outer"):
        a = d.get(which, {})
        lines.append(
            f"  {which}: "
            + (f"{a.get('sampleName')} @ {a.get('frequencyHz', 0) / 1e6:.3f} MHz "
               f"bw={a.get('bandwidthKhz')} repeats={a.get('repeats')} "
               f"{'READY' if a.get('ready') else 'not ready'}"
               if a.get("assigned") else "unassigned")
        )
    emit(args, d, "\n".join(lines))


def cmd_fire(args):
    d = api(args, "/api/gate/fire", {"which": args.which}, method="POST")
    emit(args, d, f"fired {d.get('which')} gate: {d.get('sampleName')}")


def cmd_sample(args):
    if args.action == "list":
        return cmd_samples(args)
    if not args.name:
        raise ApiError(f"sample {args.action} needs a name")
    if args.action == "save":
        d = api(args, "/api/sample/save", {"name": args.name})
        return emit(args, d, f"saved {d.get('name')} @ {d.get('frequencyHz', 0) / 1e6:.6f} MHz")
    if args.action == "load":
        d = api(args, "/api/sample/load", {"name": args.name})
        return emit(args, d, f"loaded {d.get('name')}")
    if args.action == "decode":
        d = api(args, "/api/sample/decode", {"name": args.name})
        return emit(args, d, f"encoding: {d.get('encoding')}  protocol: {d.get('protocol_candidate')}")
    if args.action == "delete":
        d = api(args, "/api/sample/delete", {"name": args.name})
        return emit(args, d, f"deleted {args.name}: ok={d.get('ok')}")
    if args.action == "play":
        params = {"name": args.name, "repeat": args.repeat, "invert": 1 if args.invert else 0}
        d = api(args, "/api/sample/play", params, method="GET")
        return emit(args, d, f"playing {args.name} x{args.repeat}{' inverted' if args.invert else ''}")


def cmd_watch(args):
    print(f"watching {args.host}  (ctrl-c to stop)")
    try:
        while True:
            try:
                x = call(args.host, "/api/status", timeout=args.timeout)
                print(
                    f"{time.strftime('%H:%M:%S')}  mode={x.get('mode'):<10} "
                    f"busy={str(x.get('busy')):<5} heap={x.get('heap')} "
                    f"pulses={x.get('pulses')} sweep={x.get('sweepCount')} "
                    f"{x.get('frequencyHz', 0) / 1e6:.4f}MHz "
                    f"wifi={'up' if x.get('wifiConnected') else 'down'} "
                    f"apClients={x.get('apClients')}"
                )
            except ApiError as e:
                print(f"{time.strftime('%H:%M:%S')}  {e}")
            time.sleep(args.interval)
    except KeyboardInterrupt:
        pass


def cmd_raw(args):
    params = dict(p.split("=", 1) for p in args.param)
    d = api(args, args.path, params or None, method=args.method)
    json.dump(d, sys.stdout, indent=2)
    sys.stdout.write("\n")


def build_parser():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default=DEFAULT_HOST, help=f"device host (default {DEFAULT_HOST})")
    p.add_argument("--json", action="store_true", help="print raw JSON instead of a summary")
    p.add_argument("--timeout", type=float, default=10.0, help="per-request HTTP timeout, seconds")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("status").set_defaults(func=cmd_status)
    sub.add_parser("selftest").set_defaults(func=cmd_selftest)

    t = sub.add_parser("tune")
    t.add_argument("mhz", type=float)
    t.add_argument("bw", nargs="?", type=int, default=650, choices=(58, 270, 650))
    t.set_defaults(func=cmd_tune)

    s = sub.add_parser("sweep")
    s.add_argument("start", type=float, help="start MHz")
    s.add_argument("stop", type=float, help="stop MHz")
    s.add_argument("--step", type=float, default=20.0, help="step kHz (default 20)")
    s.add_argument("--dwell", type=int, default=3)
    s.add_argument("--bw", type=int, default=650, choices=(58, 270, 650))
    s.add_argument("--wait", type=float, default=30.0, help="seconds to wait for the sweep to finish")
    s.set_defaults(func=cmd_sweep)

    r = sub.add_parser("record")
    r.add_argument("ms", type=int)
    r.add_argument("--histogram", action="store_true")
    r.add_argument("--decode", action="store_true")
    r.add_argument("--wait", type=float, default=10.0, help="extra seconds to wait past `ms` for idle")
    r.set_defaults(func=cmd_record)

    sub.add_parser("histogram").set_defaults(func=cmd_histogram)
    sub.add_parser("pulses").set_defaults(func=cmd_pulses)
    sub.add_parser("decode").set_defaults(func=cmd_decode)
    sub.add_parser("samples").set_defaults(func=cmd_samples)
    sub.add_parser("gate").set_defaults(func=cmd_gate)

    f = sub.add_parser("fire")
    f.add_argument("which", choices=("inner", "outer"))
    f.set_defaults(func=cmd_fire)

    sm = sub.add_parser("sample", help="operate on saved samples")
    sm.add_argument("action", choices=("list", "save", "load", "decode", "delete", "play"))
    sm.add_argument("name", nargs="?", help="sample name (not needed for list)")
    sm.add_argument("--repeat", type=int, default=1, help="play: repeat count (1-10)")
    sm.add_argument("--invert", action="store_true", help="play: invert logic")
    sm.set_defaults(func=cmd_sample)

    w = sub.add_parser("watch", help="poll /api/status until ctrl-c")
    w.add_argument("--interval", type=float, default=2.0)
    w.set_defaults(func=cmd_watch)

    raw = sub.add_parser("raw", help="call an arbitrary endpoint")
    raw.add_argument("path")
    raw.add_argument("param", nargs="*", help="key=value query params")
    raw.add_argument("--method", default="GET")
    raw.set_defaults(func=cmd_raw)

    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        args.func(args)
    except ApiError as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()

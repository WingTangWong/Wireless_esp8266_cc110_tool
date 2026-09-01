"""In-process fake of the firmware's HTTP API, for exercising tools/rfprobe.py
without hardware. Not a full emulator - just enough plausible JSON.
"""

import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse


class _State:
    def __init__(self):
        self.mode = "idle"
        self.freq = 318_000_000
        self.bw = 650
        self.busy_until = 0.0
        self.pulses = 0
        self.sweep = []
        self.samples = {"sample01": (318_000_000, 50, 120_000)}
        self.gate_enabled = True

    @property
    def busy(self):
        return time.time() < self.busy_until

    def go_busy(self, mode, secs):
        self.mode = mode
        self.busy_until = time.time() + secs

    def settle(self):
        if not self.busy and self.mode != "idle":
            self.mode = "idle"


def _handler(state):
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _json(self, obj, code=200):
            b = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)

        def do_POST(self):
            self.do_GET()

        def do_GET(self):
            state.settle()
            u = urlparse(self.path)
            q = {k: v[0] for k, v in parse_qs(u.query).items()}
            p = u.path

            if p == "/api/selftest":
                return self._json({
                    "ok": True, "firmwareVersion": "0.0-test", "firmwareBuild": "x",
                    "checks": {"spi": True, "radioVersionSane": True, "littlefs": True,
                               "heap": True, "network": True},
                    "radioPartnum": 0, "radioVersion": 20, "heap": 30000,
                    "fsUsedBytes": 2048, "fsTotalBytes": 1_024_000,
                    "mode": state.mode, "busy": state.busy,
                })
            if p == "/api/status":
                return self._json({
                    "ok": True, "radio": True, "firmwareVersion": "0.0-test",
                    "firmwareBuild": "x", "radioPartnum": 0, "radioVersion": 20,
                    "mode": state.mode, "wifiConnected": True, "wifiSsid": "n",
                    "wifiIp": "1.2.3.4", "wifiRssi": -50, "hostname": "cc1101.local",
                    "apActive": True, "apSsid": "cc1101-setup", "apIp": "192.168.4.1",
                    "apClients": 0, "frequencyHz": state.freq, "bandwidthKhz": state.bw,
                    "gdo0Pin": 4, "gdo2Pin": 5, "heap": 30000, "fsUsedBytes": 2048,
                    "fsTotalBytes": 1_024_000, "pulses": state.pulses,
                    "captureDone": state.pulses > 0, "captureDurationUs": 20000,
                    "captureTruncated": False, "maxPulses": 3072,
                    "sweepDone": bool(state.sweep) and not state.busy,
                    "sweepCount": len(state.sweep), "busy": state.busy,
                })
            if p == "/api/tune":
                hz = int(q.get("hz", 0))
                if not 300_000_000 <= hz <= 348_000_000:
                    return self._json({"ok": False, "error": "frequency must be 300-348 MHz"}, 400)
                state.freq = hz
                return self._json({"ok": True, "frequencyHz": hz})
            if p == "/api/sweep/start":
                a, b, s = int(q["start"]), int(q["stop"]), int(q["step"])
                state.sweep = [{"hz": h, "rssi": -95 + (h % 11)} for h in range(a, b + 1, s)]
                state.go_busy("sweeping", 0.3)
                return self._json({"ok": True})
            if p == "/api/sweep/data":
                pts = state.sweep
                rss = [x["rssi"] for x in pts]
                summ = None
                if pts:
                    mx = max(rss)
                    summ = {"rssiMinDbm": min(rss), "rssiMaxDbm": mx,
                            "rssiMeanDbm": round(sum(rss) / len(rss), 1),
                            "peakHz": pts[rss.index(mx)]["hz"]}
                return self._json({"ok": True, "count": len(pts), "points": pts, "summary": summ})
            if p == "/api/capture/start":
                state.go_busy("recording", 0.3)
                state.pulses = 16
                return self._json({"ok": True})
            if p == "/api/capture/pulses":
                pulses = []
                for b in (1, 0, 1, 1, 0, 0, 1, 0):
                    pulses += [[500, 1], [1500 if b else 500, 0]]
                return self._json({"ok": True, "frequencyHz": state.freq,
                                   "durationUs": 20000, "truncated": False,
                                   "count": len(pulses), "pulses": pulses})
            if p == "/api/capture/histogram":
                counts = [8, 4, 4] + [0] * 25
                high = [4, 2, 2] + [0] * 25
                return self._json({"binWidthUs": 100, "counts": counts, "high": high,
                                   "low": [c - h for c, h in zip(counts, high)],
                                   "labels": [str(i * 100) for i in range(28)]})
            if p in ("/api/decode/current", "/api/sample/decode"):
                return self._json({"ok": True, "frequencyHz": state.freq, "pulses": 16,
                                   "encoding": "pulse-distance / PPM-style", "short_us": 500.0,
                                   "long_us": 1500.0, "ratio": 3.0, "sync_gap_threshold_us": 6000,
                                   "high_clusters_us": [500, 500], "low_clusters_us": [500, 1500],
                                   "candidate_bits": "1011", "candidate_bits_inverted": "0100",
                                   "protocol_candidate": None})
            if p == "/api/samples":
                return self._json({"ok": True, "samples": [
                    {"name": n, "frequencyHz": f, "pulseCount": pc, "durationUs": d}
                    for n, (f, pc, d) in state.samples.items()]})
            if p == "/api/sample/save":
                n = q.get("name", "sample")
                state.samples[n] = (state.freq, state.pulses or 10, 20000)
                return self._json({"ok": True, "name": n, "frequencyHz": state.freq})
            if p == "/api/sample/load":
                n = q.get("name", "")
                if n not in state.samples:
                    return self._json({"ok": False, "error": "sample not found"}, 400)
                return self._json({"ok": True, "name": n, "frequencyHz": state.samples[n][0]})
            if p == "/api/sample/delete":
                state.samples.pop(q.get("name", ""), None)
                return self._json({"ok": True})
            if p == "/api/sample/rename":
                fr, to = q.get("from", ""), q.get("to", "")
                if fr not in state.samples:
                    return self._json({"ok": False, "error": "source sample not found"}, 400)
                if to in state.samples:
                    return self._json({"ok": False, "error": "exists"}, 400)
                state.samples[to] = state.samples.pop(fr)
                return self._json({"ok": True, "name": to})
            if p == "/api/sample/play":
                state.go_busy("playback", 0.2)
                return self._json({"ok": True})
            if p == "/api/gate":
                empty = {"assigned": False, "sampleName": "", "sampleExists": False,
                         "frequencyHz": 0, "bandwidthKhz": 0, "txPowerDbm": 12,
                         "repeats": 0, "invert": False, "ready": False}
                return self._json({"ok": True, "enabled": state.gate_enabled,
                                   "txPowerForcedDbm": 12, "minRepeats": 4,
                                   "lastFireError": "",
                                   "inner": dict(empty), "outer": dict(empty)})
            if p == "/api/gate/enable":
                state.gate_enabled = q.get("on", "1") != "0"
                return self._json({"ok": True, "enabled": state.gate_enabled})
            return self._json({"ok": False, "error": "not found"}, 404)

    return H


class FakeDevice:
    def __init__(self):
        self.state = _State()
        self._srv = ThreadingHTTPServer(("127.0.0.1", 0), _handler(self.state))
        self.host = f"127.0.0.1:{self._srv.server_address[1]}"

    def __enter__(self):
        threading.Thread(target=self._srv.serve_forever, daemon=True).start()
        return self

    def __exit__(self, *exc):
        self._srv.shutdown()
        self._srv.server_close()

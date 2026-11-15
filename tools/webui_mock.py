#!/usr/bin/env python3
"""Mock of the launcher's web API, for editing/testing main/webui.html without hardware.

    python3 tools/webui_mock.py [port]      then open http://localhost:8080/

Mirrors the device: POSTs need the X-Basecamp header, uploads are checked like
the real slot writer (ESP image header, ESP32-S3, app descriptor), a slot that
holds an app needs replace=1, GitHub installs run as a fake background job.
"""
import json, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse, parse_qs

PAGE = Path(__file__).resolve().parent.parent / "main" / "webui.html"
LOCK = threading.Lock()

def slot(i, name="", ver="", proj="", col="", src="", size=0, st="empty"):
    return dict(i=i, st=st, card=st != "empty", name=name, ver=ver, proj=proj, detail="" if st == "app" else "empty",
                tag="", src=src, col=col or ["ff7a1a", "18c8b0", "c850f0"][i], chg=False, size=size)

STATE = dict(
    v="0.3.0-mock", busy=False,
    slots=[slot(0, "PrintSphere", "v1.6.2", "printsphere_idf", src="cptkirki/PrintSphere", size=2997088, st="app"),
           slot(1, "Capsule Radar", "v1.6.0", "arduino-lib-builder", src="socquique/capsule-radar", size=3015840, st="app"),
           slot(2)],
    order=[0, 1, 2],
    wifi=dict(has=False, ssid="", st="none"),
    job=dict(active=False, slot=-1, pct=0, text="", err="", seq=0),
    presets=[dict(name="PrintSphere", repo="cptkirki/PrintSphere", prefix="printsphere_ota", suffix=".bin", exclude="2.8c", project="printsphere", marker="esp32_s3_touch_amoled_1_75"),
             dict(name="Capsule Radar", repo="socquique/capsule-radar", prefix="CapsuleRadar-ota.bin", suffix=".bin", exclude="", project="arduino-lib-builder", marker="CO5300"),
             dict(name="LyricFlash", repo="skiiirt1983/LyricFlash", prefix="lyricflash-ota.bin", suffix=".bin", exclude="", project="lyricflash", marker="esp32_s3_touch_amoled_1_75")],
)
SCAN_UNTIL = 0.0

def view():
    s = json.loads(json.dumps(STATE))
    s["free"] = next((x["i"] for x in s["slots"] if x["st"] == "empty"), -1)
    return s

def run_job(slot_i, name, repo):
    def go():
        with LOCK:
            STATE["job"].update(active=True, slot=slot_i, pct=0, text="checking GitHub", err="")
        for p in range(0, 101, 10):
            time.sleep(0.25)
            with LOCK:
                STATE["job"].update(pct=p, text="downloading v1.0.0")
        with LOCK:
            STATE["slots"][slot_i] = slot(slot_i, name, "v1.0.0", "demo", src=repo, size=2900000, st="app")
            order = [o for o in STATE["order"] if o != slot_i] + [slot_i]
            STATE["order"] = order
            STATE["job"].update(active=False, err="", seq=STATE["job"]["seq"] + 1)
    threading.Thread(target=go, daemon=True).start()

def check_header(d):
    if len(d) < 48: return "file too small to be an app image"
    if d[0] != 0xE9: return "not an ESP32 .bin (zip, elf or an error page?)"
    chip = d[12] | d[13] << 8
    if chip != 9: return "built for another chip (id %d), needs ESP32-S3" % chip
    if int.from_bytes(d[32:36], "little") != 0xABCD5432:
        return "no app descriptor: use the app-only OTA .bin, not a merged/full-flash or bootloader image"
    return None

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def out(self, code, obj, ctype="application/json"):
        b = obj if isinstance(obj, bytes) else json.dumps(obj).encode()
        self.send_response(code); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def err(self, code, msg): self.out(code, {"error": msg})
    def do_GET(self):
        u = urlparse(self.path)
        if u.path == "/": return self.out(200, PAGE.read_bytes(), "text/html")
        if u.path == "/api/state":
            with LOCK: return self.out(200, view())
        if u.path == "/api/wifi/scan":
            running = time.time() < SCAN_UNTIL
            nets = [] if running else [dict(ssid="HomeNet", rssi=-48, open=False), dict(ssid="Neighbour 5G?", rssi=-71, open=False), dict(ssid="CoffeeShop", rssi=-80, open=True)]
            return self.out(200, dict(running=running, nets=nets))
        self.send_response(302); self.send_header("Location", "http://192.168.4.1/"); self.end_headers()   # captive portal redirect
    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n)
    def do_POST(self):
        global SCAN_UNTIL
        u = urlparse(self.path); q = {k: v[0] for k, v in parse_qs(u.query).items()}
        data = self.body()
        if "X-Basecamp" not in self.headers: return self.err(403, "missing X-Basecamp header")
        with LOCK:
            if u.path == "/api/upload":
                if STATE["job"]["active"]: return self.err(409, "the device is busy, try again in a moment")
                msg = check_header(data)
                if msg: return self.err(400, msg)
                i = int(q.get("slot", -1))
                if i < 0: i = next((x["i"] for x in STATE["slots"] if x["st"] == "empty"), -1)
                if i < 0: return self.err(409, "no free slot: choose one to replace")
                if STATE["slots"][i]["card"] and q.get("replace") != "1":
                    return self.err(409, "slot %d holds '%s': confirm to replace it" % (i + 1, STATE["slots"][i]["name"]))
                name = q.get("name") or "Uploaded App"
                STATE["slots"][i] = slot(i, name, "1.0", "custom", col=q.get("color", "ff7a1a"), size=len(data), st="app")
                STATE["order"] = [o for o in STATE["order"] if o != i] + [i]
                resp = dict(ok=True, slot=i, name=name)
                if b"co5300" not in data.lower():
                    resp["warn"] = "No CO5300 display driver found in this image: it may not be built for this board."
                return self.out(200, resp)
            j = json.loads(data or b"{}")
            if u.path == "/api/order":
                if sorted(j.get("order", [])) != [0, 1, 2]: return self.err(400, "order must list all slots")
                STATE["order"] = j["order"]; return self.out(200, {"ok": True})
            if u.path == "/api/rename":
                s = STATE["slots"][j["slot"]]
                if j.get("name"): s["name"] = j["name"][:31]
                if j.get("color"): s["col"] = j["color"]
                return self.out(200, {"ok": True})
            if u.path == "/api/delete":
                STATE["slots"][j["slot"]] = slot(j["slot"]); return self.out(200, {"ok": True})
            if u.path == "/api/install":
                if STATE["job"]["active"]: return self.err(409, "the device is busy, try again in a moment")
                if "update" in j:
                    s = STATE["slots"][j["update"]]; run_job(j["update"], s["name"], s["src"]); return self.out(200, {"ok": True})
                if "/" not in j.get("repo", ""): return self.err(400, "repository must look like owner/name")
                i = j.get("slot", -1)
                if i < 0: i = next((x["i"] for x in STATE["slots"] if x["st"] == "empty"), -1)
                if i < 0: return self.err(409, "no free slot: choose one to replace")
                if STATE["slots"][i]["card"] and not j.get("replace"): return self.err(409, "slot %d holds '%s': confirm to replace it" % (i + 1, STATE["slots"][i]["name"]))
                run_job(i, j.get("name") or j["repo"].split("/")[1], j["repo"]); return self.out(200, {"ok": True})
            if u.path == "/api/wifi":
                if len(j.get("pass", "")) not in (0,) and len(j["pass"]) < 8: return self.err(400, "Wi-Fi passwords are at least 8 characters")
                STATE["wifi"] = dict(has=True, ssid=j["ssid"], st="connected"); return self.out(200, {"ok": True})
            if u.path == "/api/wifi/forget":
                STATE["wifi"] = dict(has=False, ssid="", st="none"); return self.out(200, {"ok": True})
            if u.path == "/api/wifi/scan":
                SCAN_UNTIL = time.time() + 1.2; return self.out(200, {"ok": True})
        self.err(404, "not found")

if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    print("mock device on http://localhost:%d/" % port)
    ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()

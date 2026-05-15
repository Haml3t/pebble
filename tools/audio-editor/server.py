#!/usr/bin/env python3
"""
Local audio editor for Glance metronome recordings.

Workflow:
  1. Connect the Android phone over USB (`adb devices` should list it).
  2. Run this script: it `adb pull`s the m4a + sidecar JSON files into the
     `recordings/` dir alongside this script, then serves the editor UI on
     http://localhost:8765/.
  3. Open the URL, pick a recording, scrub through, drag-select a range,
     hit "Save selection as WAV".

The editor is a static HTML/JS app — this script is just a thin adb wrapper
plus a static file server, with a single `/api/files` endpoint so the UI
doesn't have to scrape a directory listing.
"""
import argparse
import http.server
import json
import os
import re
import shutil
import socketserver
import subprocess
import sys
import threading
import webbrowser
from pathlib import Path

PHONE_DIR = "/sdcard/Android/data/com.dsugarman.glance/files/Music/metronome"
DEFAULT_PORT = 8765
HERE = Path(__file__).resolve().parent
LOCAL_DIR = HERE / "recordings"


def adb_pull():
    if not shutil.which("adb"):
        print("adb not on PATH — skipping pull, serving whatever's in recordings/")
        return
    LOCAL_DIR.mkdir(exist_ok=True)
    # `adb pull <dir> <local>` copies the entire directory; we use `-a` to
    # preserve mtimes so file ordering by date stays meaningful.
    cmd = ["adb", "pull", "-a", PHONE_DIR + "/.", str(LOCAL_DIR)]
    print(f"$ {' '.join(cmd)}")
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        # Not fatal — just warn. The phone dir may not exist yet if no
        # recordings have been made, or adb may not be authorized.
        print(f"adb pull warning: {res.stderr.strip()}")
    else:
        # Strip the noisy "pulled N files" line down to just the count.
        last = res.stdout.strip().splitlines()[-1] if res.stdout.strip() else ""
        print(last or "(no output)")


META_PATH = LOCAL_DIR / "metadata.json"


def _load_meta():
    if META_PATH.exists():
        try:
            return json.loads(META_PATH.read_text())
        except Exception:
            pass
    return {"starred": []}


def _save_meta(meta):
    LOCAL_DIR.mkdir(exist_ok=True)
    META_PATH.write_text(json.dumps(meta, indent=2))


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(HERE), **kwargs)

    def do_GET(self):
        if self.path == "/api/files":
            return self._send_file_list()
        return super().do_GET()

    def do_POST(self):
        # Endpoints, matching the Android EditorServer counterpart:
        #   POST /api/save-clip?name=foo.wav  — write request body as WAV
        #   POST /api/package?name=foo.m4a    — build sharable .zip on disk
        from urllib.parse import urlparse, parse_qs
        parsed = urlparse(self.path)
        if parsed.path == "/api/package":
            return self._make_package(parse_qs(parsed.query))
        if parsed.path == "/api/delete":
            return self._delete_recording(parse_qs(parsed.query))
        if parsed.path == "/api/star":
            return self._set_star(parse_qs(parsed.query))
        if parsed.path != "/api/save-clip":
            self.send_error(404)
            return
        qs = parse_qs(parsed.query)
        name = (qs.get("name") or [""])[0]
        if not name or "/" in name or ".." in name:
            self.send_error(400, "bad name")
            return
        if not name.endswith(".wav"):
            name += ".wav"
        n = int(self.headers.get("Content-Length", "0"))
        out = LOCAL_DIR / name
        LOCAL_DIR.mkdir(exist_ok=True)
        with open(out, "wb") as f:
            remaining = n
            while remaining > 0:
                chunk = self.rfile.read(min(64 * 1024, remaining))
                if not chunk:
                    break
                f.write(chunk)
                remaining -= len(chunk)
        body = json.dumps({"name": name, "bytes": out.stat().st_size}).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _delete_recording(self, qs):
        name = (qs.get("name") or [""])[0]
        if not name or "/" in name or ".." in name:
            self.send_error(400, "bad name")
            return
        # Allow deleting either a parent m4a (also wipes sidecar + clips)
        # or a single wav clip.
        if name.endswith(".m4a"):
            base = LOCAL_DIR / name
            stem = base.stem
            removed = []
            for f in [base, base.with_suffix(".json")]:
                if f.exists():
                    f.unlink(); removed.append(f.name)
            for clip in LOCAL_DIR.glob(stem + "*.wav"):
                clip.unlink(); removed.append(clip.name)
        elif name.endswith(".wav"):
            f = LOCAL_DIR / name
            if f.exists():
                f.unlink(); removed = [f.name]
            else:
                removed = []
        else:
            self.send_error(400, "must be .m4a or .wav")
            return
        # Also remove from starred list.
        meta = _load_meta()
        meta["starred"] = [s for s in meta.get("starred", []) if s not in removed]
        _save_meta(meta)
        body = json.dumps({"removed": removed}).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _set_star(self, qs):
        name = (qs.get("name") or [""])[0]
        value = (qs.get("value") or ["true"])[0].lower() == "true"
        if not name or "/" in name or ".." in name:
            self.send_error(400)
            return
        meta = _load_meta()
        starred = set(meta.get("starred", []))
        if value: starred.add(name)
        else:     starred.discard(name)
        meta["starred"] = sorted(starred)
        _save_meta(meta)
        body = json.dumps({"name": name, "starred": value}).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _make_package(self, qs):
        """Bundle an .m4a and its .json sidecar into a .zip in recordings/share/.
        The receiver unzips into their own recordings/ and views via the same
        editor — instructions live in tools/audio-editor/README.md."""
        import zipfile
        name = (qs.get("name") or [""])[0]
        if not name or "/" in name or ".." in name or not name.endswith(".m4a"):
            self.send_error(400, "bad name")
            return
        m4a = LOCAL_DIR / name
        if not m4a.exists():
            self.send_error(404, "not found")
            return
        sidecar = m4a.with_suffix(".json")
        # Save next to the m4a (not in a share/ subdir) so the Android side's
        # MetronomeFileProvider — which resolves names against the recordings
        # dir root — can share the zip by its filename via the bridge.
        # The file-list endpoint filters to *.m4a, so zips don't clutter the UI.
        zip_path = LOCAL_DIR / (m4a.stem + ".zip")
        with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
            z.write(m4a, m4a.name)
            if sidecar.exists():
                z.write(sidecar, sidecar.name)
        body = json.dumps({
            "name": zip_path.name,
            "path": str(zip_path.relative_to(HERE)),
            "abs_path": str(zip_path),
            "bytes": zip_path.stat().st_size,
        }).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_file_list(self):
        items = []
        meta = _load_meta()
        starred = set(meta.get("starred", []))
        if LOCAL_DIR.exists():
            for m4a in sorted(LOCAL_DIR.glob("*.m4a"), reverse=True):
                sidecar = m4a.with_suffix(".json")
                # Parse BPM range from filename — much faster than decoding
                # the file to read its metadata, and the truth is embedded
                # there anyway since MetronomeService writes it.
                m = re.search(r"_(\d+)(?:-(\d+))?bpm", m4a.stem)
                bpm_lo = int(m.group(1)) if m else None
                bpm_hi = int(m.group(2)) if (m and m.group(2)) else bpm_lo
                # Clips: any .wav whose name starts with the parent's stem.
                # The save-clip endpoint follows this naming convention so
                # clips slot into the right parent automatically.
                clips = []
                for wav in sorted(LOCAL_DIR.glob(m4a.stem + "*.wav")):
                    clips.append({
                        "name": wav.name,
                        "size": wav.stat().st_size,
                        "starred": wav.name in starred,
                    })
                items.append({
                    "name": m4a.name,
                    "size": m4a.stat().st_size,
                    "mtime": m4a.stat().st_mtime,
                    "has_sidecar": sidecar.exists(),
                    "bpm_lo": bpm_lo,
                    "bpm_hi": bpm_hi,
                    "starred": m4a.name in starred,
                    "clips": clips,
                })
        body = json.dumps(items).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        # Quieter default — the server is local and not interesting to log.
        if "/api/" in fmt % args or ".m4a" in fmt % args:
            sys.stderr.write("[%s] %s\n" % (self.log_date_time_string(),
                                            fmt % args))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--no-pull", action="store_true",
                    help="Skip the adb pull step; serve whatever's already local.")
    ap.add_argument("--no-open", action="store_true",
                    help="Don't auto-open the browser.")
    args = ap.parse_args()

    if not args.no_pull:
        adb_pull()

    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("127.0.0.1", args.port), Handler) as httpd:
        url = f"http://127.0.0.1:{args.port}/"
        print(f"Serving on {url}  (Ctrl-C to stop)")
        if not args.no_open:
            threading.Timer(0.5, lambda: webbrowser.open(url)).start()
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nbye")


if __name__ == "__main__":
    main()

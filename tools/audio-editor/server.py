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


# === Bidirectional sync ===================================================

def _adb_ls():
    """Files (not directories) directly under PHONE_DIR. Returns None if
    adb is unavailable / phone unreachable. `find -maxdepth 1 -type f`
    (default output = one full path per line) so we don't try to mirror
    sub-directories like `share/`. We avoid `-printf '%f\\n'` because the
    remote sh eats the literal `\\n` before find sees it, producing one
    giant joined string."""
    if not shutil.which("adb"):
        return None
    res = subprocess.run(
        ["adb", "shell", "find", PHONE_DIR,
         "-maxdepth", "1", "-type", "f"],
        capture_output=True, text=True)
    if res.returncode != 0:
        return None
    prefix = PHONE_DIR.rstrip("/") + "/"
    names = set()
    for ln in res.stdout.splitlines():
        ln = ln.strip()
        if not ln:
            continue
        names.add(ln[len(prefix):] if ln.startswith(prefix) else ln)
    return names


def _is_syncable(name: str) -> bool:
    """Skip files we never want to round-trip: pending recordings (mid-write
    on the phone side) and any hidden/tmp scratch files we use during merge."""
    if name.endswith("_pending.m4a") or name.endswith("_pending.json"):
        return False
    if name.startswith("_phone_"):
        return False
    return True


def _adb_push(local_path: Path, remote_name: str) -> bool:
    cmd = ["adb", "push", "-p", str(local_path), PHONE_DIR + "/" + remote_name]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"adb push {remote_name} failed: {res.stderr.strip()}")
        return False
    return True


def _adb_pull_one(remote_name: str, local_path: Path) -> bool:
    cmd = ["adb", "pull", "-a", PHONE_DIR + "/" + remote_name, str(local_path)]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"adb pull {remote_name} failed: {res.stderr.strip()}")
        return False
    return True


def _merge_metadata(local_meta: dict, phone_path: Path) -> dict:
    """Merge starred (union), names (last-writer-wins, preferring non-empty),
    and durations (largest value — they should be equal but take the bigger
    in case one side decoded a complete file and the other a partial)."""
    phone_meta = {"starred": [], "names": {}, "durations": {}}
    if phone_path.exists():
        try:
            data = json.loads(phone_path.read_text())
            phone_meta["starred"] = data.get("starred", [])
            phone_meta["names"] = data.get("names", {}) or {}
            phone_meta["durations"] = data.get("durations", {}) or {}
        except Exception:
            pass
    starred = sorted(set(local_meta.get("starred", []))
                     | set(phone_meta.get("starred", [])))
    names = {}
    for src in (phone_meta["names"], local_meta.get("names", {}) or {}):
        # Local takes precedence on conflict — the laptop is usually where
        # renames happen via the rename prompt.
        for k, v in src.items():
            if v: names[k] = v
    durations = {}
    for src in (phone_meta["durations"], local_meta.get("durations", {}) or {}):
        for k, v in src.items():
            existing = durations.get(k)
            if existing is None or (v and v > existing):
                durations[k] = v
    return {"starred": starred, "names": names, "durations": durations}


def _merge_markers(local_path: Path, phone_path: Path) -> dict:
    """Merge two .markers.json files: bucket by t_ms, prefer the entry with
    a non-empty note. Editor-saved notes always beat the empty stub a watch
    UP-press would seed."""
    def _load(p):
        if not p.exists():
            return []
        try:
            return json.loads(p.read_text()).get("markers", []) or []
        except Exception:
            return []
    a = _load(local_path)
    b = _load(phone_path)
    by_t = {}
    for src in (a, b):
        for m in src:
            try:
                t = int(m.get("t_ms", 0))
            except Exception:
                continue
            note = str(m.get("note", ""))[:500]
            existing = by_t.get(t)
            # Prefer the entry with a longer note (non-empty wins over empty,
            # later edits over earlier truncations).
            if existing is None or len(note) > len(existing["note"]):
                by_t[t] = {"t_ms": t, "note": note}
    return {"markers": sorted(by_t.values(), key=lambda m: m["t_ms"])}


def sync_with_phone() -> dict:
    """Two-way mirror between LOCAL_DIR and PHONE_DIR via adb. Returns a
    summary dict the API endpoint can ship back to the UI."""
    summary = {"pulled": [], "pushed": [], "merged": [], "ok": False,
               "error": None}
    if not shutil.which("adb"):
        summary["error"] = "adb not on PATH"
        return summary
    LOCAL_DIR.mkdir(exist_ok=True)

    phone_files = _adb_ls()
    if phone_files is None:
        summary["error"] = "phone not reachable via adb"
        return summary

    local_files = {f.name for f in LOCAL_DIR.iterdir()
                   if f.is_file() and f.name != "metadata.json"
                   and not f.name.endswith(".markers.json")
                   and _is_syncable(f.name)}
    phone_files = {n for n in phone_files if _is_syncable(n)}

    # 1. Files only on phone → pull. (m4a, json sidecars, zip packages, wavs
    # the phone made via its own editor.)
    for name in phone_files - local_files:
        if name == "metadata.json" or name.endswith(".markers.json"):
            continue  # handled by merge step below
        if _adb_pull_one(name, LOCAL_DIR / name):
            summary["pulled"].append(name)

    # 2. Files only on laptop → push.
    for name in local_files - phone_files:
        if _adb_push(LOCAL_DIR / name, name):
            summary["pushed"].append(name)

    # 3. metadata.json (starred): union both sides, write to both.
    local_meta = _load_meta()
    # Stash the phone copy in a tmp file we can read+merge without
    # clobbering the laptop's copy.
    tmp = LOCAL_DIR / "_phone_metadata.json"
    if "metadata.json" in phone_files and _adb_pull_one("metadata.json", tmp):
        merged = _merge_metadata(local_meta, tmp)
        tmp.unlink()
    else:
        merged = local_meta
    _save_meta(merged)
    if _adb_push(META_PATH, "metadata.json"):
        summary["merged"].append("metadata.json")

    # 4. *.markers.json: merge each side's set by t_ms.
    laptop_markers = {f.name for f in LOCAL_DIR.iterdir()
                      if f.name.endswith(".markers.json")}
    phone_markers = {n for n in phone_files if n.endswith(".markers.json")}
    for name in laptop_markers | phone_markers:
        local = LOCAL_DIR / name
        phone_tmp = LOCAL_DIR / ("_phone_" + name)
        if name in phone_markers:
            _adb_pull_one(name, phone_tmp)
        merged = _merge_markers(local, phone_tmp)
        if phone_tmp.exists():
            phone_tmp.unlink()
        local.write_text(json.dumps(merged, indent=2))
        if _adb_push(local, name):
            summary["merged"].append(name)

    summary["ok"] = True
    return summary


META_PATH = LOCAL_DIR / "metadata.json"


def _load_meta():
    # Schema: starred = list[str], names = dict[str,str] (filename →
    # user-set display name), durations = dict[str, float] (filename →
    # seconds, populated by the editor after decode).
    if META_PATH.exists():
        try:
            data = json.loads(META_PATH.read_text())
            data.setdefault("starred", [])
            data.setdefault("names", {})
            data.setdefault("durations", {})
            return data
        except Exception:
            pass
    return {"starred": [], "names": {}, "durations": {}}


def _save_meta(meta):
    LOCAL_DIR.mkdir(exist_ok=True)
    META_PATH.write_text(json.dumps(meta, indent=2))


def _markers_path(audio_name):
    """`foo.m4a` → `foo.markers.json` next to the audio file. Keeps the
    existing sidecar (`foo.json`, written by the Android service) free of
    user edits so we never corrupt the watch-recorded event log."""
    base = LOCAL_DIR / audio_name
    return base.with_name(base.stem + ".markers.json")


def _read_editor_markers(audio_name):
    p = _markers_path(audio_name)
    if not p.exists():
        return []
    try:
        return json.loads(p.read_text()).get("markers", [])
    except Exception:
        return []


def _read_watch_markers(audio_name):
    """Extract `type=marker` events from the m4a's sidecar JSON written by
    MetronomeService when the user presses UP during recording. These
    seed the editor with markers the user hasn't annotated yet."""
    if not audio_name.endswith(".m4a"):
        return []
    sidecar = (LOCAL_DIR / audio_name).with_suffix(".json")
    if not sidecar.exists():
        return []
    try:
        s = json.loads(sidecar.read_text())
    except Exception:
        return []
    return [{"t_ms": int(e.get("t_ms", 0)), "note": "", "source": "watch"}
            for e in s.get("events", []) if e.get("type") == "marker"]


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(HERE), **kwargs)

    def do_GET(self):
        if self.path == "/api/files":
            return self._send_file_list()
        if self.path.startswith("/api/markers"):
            from urllib.parse import urlparse, parse_qs
            return self._get_markers(parse_qs(urlparse(self.path).query))
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
        if parsed.path == "/api/markers":
            return self._save_markers(parse_qs(parsed.query))
        if parsed.path == "/api/sync":
            return self._do_sync()
        if parsed.path == "/api/rename":
            return self._set_display_name(parse_qs(parsed.query))
        if parsed.path == "/api/duration":
            return self._set_duration(parse_qs(parsed.query))
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

    def _get_markers(self, qs):
        name = (qs.get("audio") or [""])[0]
        if not name or "/" in name or ".." in name:
            self.send_error(400, "bad name")
            return
        editor = _read_editor_markers(name)
        # Dedupe by t_ms: an editor-saved marker (with note) takes
        # precedence over the watch-side stub from the sidecar event log.
        seen = {m.get("t_ms") for m in editor}
        merged = list(editor)
        for wm in _read_watch_markers(name):
            if wm["t_ms"] not in seen:
                merged.append(wm)
        merged.sort(key=lambda m: m.get("t_ms", 0))
        body = json.dumps({"markers": merged}).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _save_markers(self, qs):
        name = (qs.get("audio") or [""])[0]
        if not name or "/" in name or ".." in name:
            self.send_error(400, "bad name")
            return
        n = int(self.headers.get("Content-Length", "0"))
        try:
            data = json.loads(self.rfile.read(n))
        except Exception:
            self.send_error(400, "bad json")
            return
        clean = []
        for m in data.get("markers", []):
            try:
                t = int(m.get("t_ms", 0))
            except Exception:
                continue
            # Cap note length so a runaway client can't fill the disk.
            note = str(m.get("note", ""))[:500]
            clean.append({"t_ms": t, "note": note})
        clean.sort(key=lambda m: m["t_ms"])
        p = _markers_path(name)
        p.parent.mkdir(parents=True, exist_ok=True)
        # Write empty array as `[]` rather than deleting — explicit
        # "no markers" is distinct from "never opened in editor".
        p.write_text(json.dumps({"markers": clean}, indent=2))
        body = json.dumps({"count": len(clean)}).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _set_display_name(self, qs):
        name = (qs.get("name") or [""])[0]
        display = (qs.get("display") or [""])[0]
        if not name or "/" in name or ".." in name:
            self.send_error(400, "bad name")
            return
        meta = _load_meta()
        names = meta.setdefault("names", {})
        # Empty / whitespace-only display string clears the custom name —
        # the UI falls back to the parsed timestamp/clip-suffix.
        if display.strip():
            names[name] = display.strip()[:200]
        else:
            names.pop(name, None)
        _save_meta(meta)
        body = json.dumps({"name": name,
                           "display_name": names.get(name)}).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _set_duration(self, qs):
        name = (qs.get("name") or [""])[0]
        try:
            seconds = float((qs.get("seconds") or ["0"])[0])
        except ValueError:
            self.send_error(400, "bad seconds")
            return
        if not name or "/" in name or ".." in name or seconds <= 0:
            self.send_error(400)
            return
        meta = _load_meta()
        durations = meta.setdefault("durations", {})
        durations[name] = round(seconds, 3)
        _save_meta(meta)
        body = json.dumps({"name": name,
                           "duration_sec": durations[name]}).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _do_sync(self):
        result = sync_with_phone()
        body = json.dumps(result).encode("utf-8")
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
        names = meta.get("names", {})
        durations = meta.get("durations", {})
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
                        "display_name": names.get(wav.name),
                        "duration_sec": durations.get(wav.name),
                    })
                items.append({
                    "name": m4a.name,
                    "size": m4a.stat().st_size,
                    "mtime": m4a.stat().st_mtime,
                    "has_sidecar": sidecar.exists(),
                    "bpm_lo": bpm_lo,
                    "bpm_hi": bpm_hi,
                    "starred": m4a.name in starred,
                    "display_name": names.get(m4a.name),
                    "duration_sec": durations.get(m4a.name),
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
        summary = sync_with_phone()
        if summary.get("ok"):
            print(f"sync: pulled={len(summary['pulled'])} pushed={len(summary['pushed'])} merged={len(summary['merged'])}")
        else:
            print(f"sync skipped: {summary.get('error')}")

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

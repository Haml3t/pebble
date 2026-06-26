# Opening the Glance Metronome audio editor (with all recordings loaded)

The "glance editor" is the **Glance Metronome audio editor** at `tools/audio-editor/`.
It's a tiny Python static-file server plus a pure-browser HTML/JS app for viewing,
scrubbing, and clipping the audio recordings produced by the Glance Metronome
watchapp + Android companion.

## One command

```sh
cd ~/personal/repos/pebble/tools/audio-editor
python3 server.py
```

That single command does three things:

1. **`adb pull`** — copies the `.m4a` recordings + `.json` BPM sidecars off the
   connected phone (from
   `/sdcard/Android/data/com.dsugarman.glance/files/Music/metronome/`) into the
   local `recordings/` dir. This is the "all recordings loaded" step.
2. **Serves** the editor UI on `http://127.0.0.1:8765/` (exposes one `/api/files`
   endpoint so the UI can list recordings).
3. **Opens** your browser to that URL automatically.

## Once it's open

- **URL:** http://127.0.0.1:8765/
- All recordings appear in the left sidebar — click any one to load its waveform
  + BPM track.
- The server keeps running until you stop it (Ctrl-C in its terminal). Stopping
  it frees port 8765.

## Useful flags

| Flag        | Effect                                                                                   |
|-------------|------------------------------------------------------------------------------------------|
| `--no-pull` | Skip the adb step — serve only what's already local (no phone, or viewing someone's zip) |
| `--no-open` | Don't auto-open the browser                                                              |
| `--port N`  | Serve on a different port                                                                 |

## Prerequisites

- Phone connected over USB with `adb devices` listing it.
- Python 3.8+.
- No build step, no `npm install`.

## What you can do in the editor

- Play / pause / scrub the recording.
- See the waveform aligned with the BPM step-line and ticking-on/off bands.
- Click-and-drag on either track to select a range.
- Save a selection as a 16-bit mono WAV (`💾 save selection as WAV` → lands in `recordings/`).
- Build a `📦 share package` `.zip` to send someone (lands in `recordings/share/`).

See [`README.md`](README.md) for the share-package send/receive workflow.

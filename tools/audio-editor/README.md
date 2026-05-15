# Glance Metronome — Audio Editor

A local-only tool for playing back, viewing, and clipping the audio recordings
produced by the [Glance Metronome](https://github.com/Haml3t/pebble) Pebble
watchapp + Android companion. Pure HTML/JS in your browser; the only "server"
is a tiny Python static file server (no external network calls).

## What's in a recording package

A "package" is a `.zip` containing two files:

- `2026-05-15_14-30-22_60-120bpm.m4a` — mono AAC audio captured by the phone
- `2026-05-15_14-30-22_60-120bpm.json` — sidecar with the BPM-over-time event log
  (used to draw the BPM track alongside the waveform)

Either file alone works — the editor just shows a blank BPM track if the
sidecar is missing.

## Viewing someone else's recording

When a friend sends you a `.zip` produced by their editor's "Share package"
button, do this:

```sh
# 1. Get the tool (one-time).
git clone https://github.com/Haml3t/pebble.git
cd pebble/tools/audio-editor

# 2. Drop the zip in and unpack it.
mkdir -p recordings
cp ~/Downloads/their-package.zip recordings/
cd recordings && unzip -o their-package.zip && cd ..

# 3. Run the editor.
python3 server.py --no-pull
```

That'll open your browser to `http://127.0.0.1:8765/`. The recording shows up
in the left sidebar — click to load. `--no-pull` skips the `adb` step (which
is only relevant if you have your *own* connected phone with the metronome
companion installed).

Requirements: Python 3.8+. Anything else? Nothing — the editor is pure
browser JS. No build step, no npm install.

## Things you can do with the editor

- Play / pause / scrub the recording
- See the waveform with the BPM step-line and ticking-on/off bands aligned
  underneath
- Click-and-drag on either track to select a range
- Save the selection as a 16-bit mono WAV (`💾 save selection as WAV` → ends
  up in `recordings/`)
- Build a new share package from the loaded recording (`📦 share package`)

## Sending someone else a recording

From the loaded recording's view, click `📦 share package`. The status bar
shows the resulting `.zip` path under `recordings/share/`. Attach it to an
email / Signal / wherever, and the receiver follows the steps above.

## Where recordings come from

If you're running this on your own phone-connected machine, the
`python3 server.py` startup pulls fresh recordings via `adb pull` from
`/sdcard/Android/data/com.dsugarman.glance/files/Music/metronome/` into the
local `recordings/` directory. Pass `--no-pull` to skip that step (e.g.
when looking at someone else's packages, or when no phone is connected).

The same editor UI is also embedded in the Glance NP Android app — open the
app and tap **Open audio editor** to view recordings without a laptop.

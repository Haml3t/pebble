# Glance — Pebble Time 2 watchface

A personal watchface for the Pebble Time 2 (`emery` platform) that shows: current heart rate (color-coded by Karvonen zone), local weather, Spotify/YouTube/podcast Now Playing with album-art background, and the next upcoming Google Calendar event aggregated across all visible calendars.

```
       ┌──────────────────────┐
       │         84           │   ← HR (color = cardio zone)
       │      73° Rain        │
       │                      │
       │   [album art fills   │
       │    most of screen]   │
       │                      │
       │ Galway Girl — Sheeran│
       │   10:00a · SQP       │
       └──────────────────────┘
```

## Architecture

Three pieces talking via Pebble AppMessage:

| Piece | Lives at | Job |
|-------|---------|-----|
| **Watchapp (C)** | `src/c/main.c` | Renders the four panels. Reads HR from the on-watch `HealthService`. Receives weather, calendar, now-playing strings + chunked album-art bytes via AppMessage. |
| **PebbleKit JS** | `src/pkjs/index.js`, `src/pkjs/config.json` | Runs in the Pebble phone app's JS sandbox. Polls OpenWeatherMap + Google Calendar; pushes results to the watch. Settings UI is `pebble-clay`. |
| **Android companion** | `android/` | `NotificationListenerService` + `MediaSessionManager`. Reads Now Playing metadata from ANY media app, quantizes album art to Pebble's 64-color 8bpp format, ships title/artist + chunked art bytes to the watchapp UUID via PebbleKit Android. |

The watchapp UUID is `5b5b6a8e-1f5f-4f6e-9a1f-3b9f1a2c4d5e` (hard-coded on both ends; change one, change both).

## Build & deploy

Everything runs through Docker images — no local Android SDK or Pebble SDK install.

### Watchapp (Pebble side)

```sh
./scripts/pebble build                                   # → build/project.pbw
./scripts/pebble install --phone 127.0.0.1               # install to real watch (via adb-forwarded dev connection)
```

Prerequisite for `--phone 127.0.0.1`: `adb forward tcp:9000 tcp:9000` while the phone is connected over USB and the Core Devices / Rebble app's Developer Connection is enabled.

### Android companion

```sh
./scripts/build-android                                  # → build/glance-np.apk (debug-signed)
adb install -r build/glance-np.apk                       # sideload
```

After install: open **Glance NP**, tap *Grant notification access*, enable for Glance NP in the system settings screen.

### Iteration loops

| Loop | Tool | Use when |
|------|------|---------|
| Real watch screenshot | `pebble screenshot --phone 127.0.0.1 --no-open <out.png>` | HR-dependent rendering; final visual check |
| Headless emulator | `./scripts/pebble-emu-chain "pebble build && pebble install build/project.pbw && pebble screenshot --no-open build/emu-shot.png && pebble kill"` | Layout, fonts, colors, AppMessage flow. ~70% of changes can be verified here. |
| Live logs | `./scripts/pebble-logs-loop &` → `tail -F /tmp/pebble.log` | Debugging silent failures in PKJS or watchapp |

The emulator does **not** simulate the HR sensor — `HealthMetricHeartRateBPM` returns 0. Use real hardware for HR-related changes.

## Configuration (runtime)

API keys / OAuth tokens are NOT in source. They live in PKJS `localStorage` on the phone, set via the watchface's settings page in the Pebble app:

| Service | Setup |
|---------|-------|
| OpenWeatherMap | API key from openweathermap.org → paste into "OpenWeatherMap API key" |
| Google Calendar | OAuth refresh token + client_id + client_secret → see https://developers.google.com/oauthplayground with scope `calendar.readonly`; paste each into its own field. Calendar API must be **enabled** in your Google Cloud project. |
| Now Playing | No config — works as soon as the Android companion has Notification Listener permission |

The calendar fetch aggregates across **all calendars currently visible** in your Google Calendar sidebar (filter is `selected !== false`).

## Project layout

```
.
├── src/
│   ├── c/main.c                # Watchapp C source
│   └── pkjs/
│       ├── index.js            # Phone-side JS (weather + calendar)
│       └── config.json         # Clay settings UI definition
├── android/                    # Glance NP companion app
│   └── app/src/main/
│       ├── AndroidManifest.xml
│       ├── java/com/dsugarman/glance/
│       │   ├── MainActivity.java
│       │   ├── MediaListenerService.java
│       │   └── PebblePaletteEncoder.java
│       └── res/
├── scripts/
│   ├── pebble                  # Docker wrapper for pebble-tool (real watch)
│   ├── pebble-emu              # Same but emulator + SDL=dummy
│   ├── pebble-emu-chain        # Multi-command in one container (shared QEMU)
│   ├── pebble-logs-loop        # Retry-wrapper around `pebble logs`
│   └── build-android           # Build the APK via cimg/android Docker
├── package.json                # Pebble manifest (incl. messageKeys)
├── wscript                     # Pebble waf build script
└── data/                       # Resource binaries (e.g. default_art.bin)
```

## Gotchas encountered (and the fixes)

These are non-obvious failure modes worth being aware of:

- **Pebble `*_NUMBERS` fonts have no letter glyphs.** Using e.g. `FONT_KEY_LECO_42_NUMBERS` with format strings containing letters renders them as missing-glyph icons. Either keep the format pure-numeric or switch to a font like `FONT_KEY_BITHAM_42_BOLD`.
- **No public C-side music/MediaInfo API.** `pebble.h` exposes nothing for reading the phone's MediaSession; that's why Now Playing goes via the Android companion → PebbleKit Android → AppMessage, not via PKJS or a C `music_*` call.
- **`pebble-clay` `getSettings(response, false)`** returns the dict keyed by message-key **string names** (`CFG_GOOGLE_REFRESH`). Default `true` returns it keyed by **numeric message-key IDs** (e.g. `10011`). Pick a pattern and stick to it; mismatched access yields silent-empty saves.
- **Message-key numeric IDs are auto-assigned** by the Pebble build in `package.json.messageKeys` array order, starting at 10000. After reordering, regenerate `build/js/message_keys.json` and re-sync the `MK` table in PKJS and the `KEY_*` constants in `MediaListenerService.java`.
- **HR sensor is sampled infrequently by default.** Call `health_service_set_heart_rate_sample_period(30)` and subscribe to `HealthEventHeartRateUpdate` for live updates.
- **Pebble emulator needs `SDL_VIDEODRIVER=dummy`** to boot headless inside Docker — otherwise QEMU dies with "No available video device" and the wrapper hangs forever.
- **libpebble2 has no auto-reconnect.** `pebble logs --phone …` dies the instant the phone screen sleeps or BT blips; use `./scripts/pebble-logs-loop` to retry indefinitely.
- **Docker outputs land as root.** `pebble screenshot` and `pebble build` write to the bind mount as root; `chown` back to the host user before editing the files.

## What I'd reach for, by symptom

| Symptom | First thing to try |
|---------|---------------------|
| "Nothing changed on the watch" | Switch watchfaces away and back — new builds don't always auto-restart |
| Calendar shows wrong/no event | Check Google Cloud Console → Calendar API is enabled in the project that owns the OAuth client |
| Watch shows mystery glyph icons | Font is a `*_NUMBERS` variant; check the format string |
| Album art renders scrambled | Byte format mismatch between `PebblePaletteEncoder.encode` and the watchapp's reassembly buffer; check `ART_W * ART_H` matches both sides |
| Settings save appears to no-op | Clay `getSettings` convert flag mismatch — see Gotchas |

## License & secrets

No credentials are in this repository — API keys/tokens live only on the phone in PKJS `localStorage`. The `.gitignore` covers `secrets.local.*`, `*.local.json`, `.env*`. Before committing, sanity-check with:

```sh
git diff --cached | grep -iE 'token|secret|api[_-]?key|password|bearer'
```

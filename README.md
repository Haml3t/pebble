# Glance — Pebble Time 2 watchface

Shows heart rate (color-coded by cardio zone), weather, Now Playing with album art, and the next upcoming calendar event.

```
       ┌──────────────────────────┐
       │            84            │   ← HR (color = cardio zone)
       │        73° Rain          │
       │                          │
       │     [album art fills     │
       │      most of screen]     │
       │                          │
       │Electric Funeral — Black …│
       │       10:00a · SQP       │
       └──────────────────────────┘
```

## Architecture

- **`src/c/main.c`** — C watchface. Renders the layout, reads heart rate from the on-watch Health service (with a 30-second sample period), receives weather/calendar/now-playing strings + chunked album-art bytes via `AppMessage`.
- **`src/pkjs/index.js`** — PebbleKit JS (runs in the Pebble companion app's JS sandbox on the phone). Polls OpenWeatherMap and aggregates events from every visible Google Calendar; pushes results to the watch.
- **`src/pkjs/config.json`** — Clay config UI for entering API keys.
- **`android/`** — Glance NP companion Android app. A `NotificationListenerService` reads MediaSession metadata from any media app (Spotify, YouTube Music, Pocket Casts, etc.), quantizes the album art to Pebble's 64-color 8bpp palette, and pushes title/artist + chunked art to the watchapp via PebbleKit Android.

## Prerequisites

- Docker (build images for both the Pebble SDK and Android tooling are pulled on first run)
- Android phone with the Core Devices (or Rebble) Pebble companion app installed and paired to the watch
- ADB (`sudo apt install android-tools-adb` on Debian/Ubuntu) — used to forward the dev-connection port from the phone to the laptop

## Setup

### One-time: forward the dev connection

The Core Devices / Rebble Android app exposes a developer connection on TCP 9000 of the phone. The Pebble SDK talks to it over the local network; the cleanest way to expose it from the phone is via ADB:

```sh
adb forward tcp:9000 tcp:9000
```

After that, every `--phone 127.0.0.1` invocation routes to the phone's dev connection.

### Build & install the watchface

```sh
./scripts/pebble build                                # → build/project.pbw
./scripts/pebble install --phone 127.0.0.1            # sideload to the real watch
```

### Build & install the Android companion (for Now Playing)

```sh
./scripts/build-android                               # → build/glance-np.apk
adb install -r build/glance-np.apk
```

Then on the phone:
1. Open the **Glance NP** app.
2. Tap **Grant notification access** → enable for "Glance NP" in the Android settings screen.
3. Return to the app — should show "Notification access: GRANTED" and "Pebble: CONNECTED".
4. Start playback in any media app. Title/artist + album art appear on the watch within a second or two.

## Configuration

Open the watchface settings in the Pebble companion app → tap the gear icon on Glance → fill in:

### 1. OpenWeatherMap

1. Sign up at https://openweathermap.org/api and copy your API key.
2. Paste into the **OpenWeatherMap API key** field.

### 2. Google Calendar

The watchface aggregates events from every calendar currently *visible* (selected) in your Google Calendar sidebar, and surfaces the earliest one in the next 24 hours.

1. In [Google Cloud Console](https://console.cloud.google.com/apis/credentials), create OAuth 2.0 credentials. Either application type works — Web requires `https://developers.google.com/oauthplayground` in Authorized redirect URIs.
2. **Enable the Calendar API** for that same project at https://console.cloud.google.com/apis/library/calendar-json.googleapis.com — this is a common gotcha.
3. Visit https://developers.google.com/oauthplayground, click the gear, check **Use your own OAuth credentials**, paste your client ID + secret.
4. In the scope list, expand **Calendar API v3** and check **only** `https://www.googleapis.com/auth/calendar.readonly`. (Checking the parent "Calendar API v3" header adds an invalid bare `calendar` scope that breaks authorization.)
5. Click **Authorize APIs**, sign in, then **Exchange authorization code for tokens**. Copy the `refresh_token`.
6. Back in the Glance settings page, paste each value into its own field:
    - **Refresh token**
    - **Client ID**
    - **Client secret**
7. Tap **Save**. Within a minute the watch will show your next event.

## Iteration loops

The repo includes Docker wrappers so you can verify changes without bouncing through the user:

| What you want | Command |
|--------------|---------|
| Screenshot the live watch | `./scripts/pebble screenshot --phone 127.0.0.1 --no-open build/shot.png` |
| Headless emulator end-to-end | `./scripts/pebble-emu-chain "pebble build && pebble install build/project.pbw && pebble screenshot --no-open build/emu-shot.png && pebble kill"` |
| Resilient live logs (survives BT/phone-sleep drops) | `./scripts/pebble-logs-loop &` then `tail -F /tmp/pebble.log` |

The emery emulator renders pixel-perfect at 200×228 and is great for layout/typography/AppMessage iteration — but **does not simulate the heart-rate sensor**, so `HealthMetricHeartRateBPM` returns 0 inside the emulator. For HR-related changes, use the real watch.

## Refresh cadence

- **Heart rate:** every 30 seconds (forced via `health_service_set_heart_rate_sample_period`) plus instant updates on every `HealthEventHeartRateUpdate` event.
- **Now Playing:** instant — the Android companion pushes on every MediaSession metadata change, and clears the slot when playback isn't actively playing.
- **Weather + calendar:** every minute (the watch ticks each minute and asks the JS layer to refresh).

## Files of note

| Path                                       | Purpose                                          |
|--------------------------------------------|--------------------------------------------------|
| `package.json`                             | Pebble project manifest (incl. `messageKeys`)    |
| `wscript`                                  | Pebble waf build script                          |
| `src/c/main.c`                             | Watchface rendering + AppMessage handling        |
| `src/pkjs/index.js`                        | Weather + Google Calendar fetches                |
| `src/pkjs/config.json`                     | Clay settings UI definition                      |
| `android/`                                 | Glance NP companion (NotificationListener+PebbleKit) |
| `scripts/pebble`                           | Docker wrapper for `pebble` against real hardware |
| `scripts/pebble-emu`, `pebble-emu-chain`   | Same, against the headless emery emulator        |
| `scripts/pebble-logs-loop`                 | Retry-wrapper around `pebble logs`               |
| `scripts/build-android`                    | Build the Android companion APK                  |
| `CLAUDE.md`                                | Developer/maintainer notes (gotchas, conventions) |

## Known limitations & caveats

- Now Playing requires both the **Glance NP** companion app (installed + notification-listener permission granted) and the **Core Devices / Rebble** Android companion (running, paired). The latter bridges PebbleKit data over Bluetooth to the watch.
- A few media apps don't publish album art in their MediaSession — you'll see title/artist but no thumbnail in that case.
- Google refresh tokens granted under a "Testing" OAuth consent screen expire after 7 days. Either publish the consent screen (internal use is fine) or refresh manually.
- If you change the order or contents of `messageKeys` in `package.json`, the integer IDs in `android/app/src/main/java/.../MediaListenerService.java` (`KEY_NOW_TITLE`, `KEY_NOW_ARTIST`, `KEY_ART_*`) and the `MK` table in `src/pkjs/index.js` must be re-synced from the regenerated `build/js/message_keys.json`.
- The Pebble dev connection (libpebble2) has no auto-reconnect — `pebble logs --phone` dies the instant the phone screen sleeps. `scripts/pebble-logs-loop` papers over this.

## License

MIT — see [`LICENSE`](LICENSE).

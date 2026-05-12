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

- **`src/c/main.c`** — C watchface. Renders the layout, reads heart rate from the Health API, receives weather/calendar/now-playing/art via `AppMessage`.
- **`src/pkjs/index.js`** — PebbleKit JS (runs on the paired phone). Polls OpenWeatherMap + Google Calendar; pushes results to the watch.
- **`src/pkjs/config.json`** — Clay config UI for entering API keys.
- **`android/`** — Glance NP companion Android app: NotificationListenerService that reads MediaSession metadata and pushes title/artist + 60×60 album art to the watch via PebbleKit Android.

## Prerequisites

- Docker (already verified working)
- The `rebble/pebble-sdk:latest` image (already pulled)
- A paired Pebble Time 2 with developer connection enabled (Settings → Developer)

## Build

```sh
./scripts/pebble build
```

The wrapper runs the Rebble SDK inside Docker against the current working directory. The output bundle is `build/project.pbw`.

## Install on watch

With the phone's developer connection IP visible in the Pebble app:

```sh
./scripts/pebble install --phone <phone-ip>
./scripts/pebble logs --phone <phone-ip>
```

Or install via the Rebble app store / sideload through the iOS/Android Pebble app.

## Configuration

Open the watchface settings in the Pebble phone app → tap the gear icon → fill in:

### 1. OpenWeatherMap

1. Sign up at https://openweathermap.org/api and copy your API key.
2. Paste into "OpenWeatherMap API key".

### 2. Now Playing (Glance NP Android app)

Now Playing comes from a sideloaded Android companion that reads MediaSession metadata (works for Spotify, YouTube Music, Pocket Casts, etc.) and forwards title/artist + 60×60 album art to the watch via PebbleKit Android.

Build:
```sh
./scripts/build-android        # output: build/glance-np.apk
```

Install over ADB:
```sh
adb install -r build/glance-np.apk
```

Or transfer the APK to the phone (Drive, email, etc.) and tap to install — you'll need to allow "Install unknown apps" for whichever app opens it.

After install:
1. Open the **Glance NP** app.
2. Tap **Grant notification access** → enable for "Glance NP" in the Android settings screen.
3. Return to the app — it should show "Notification access: GRANTED" and "Pebble: CONNECTED" (assuming the Pebble companion app is also running and your watch is paired).
4. Start playback in any media app. The companion will push title/artist/art to the watchface within a second or two.

### 3. Google Calendar

1. In Google Cloud Console, create OAuth 2.0 credentials (Desktop app type). Note client ID + client secret.
2. Visit https://developers.google.com/oauthplayground, click the gear icon, check "Use your own OAuth credentials", paste your client ID/secret.
3. In the left scope list, expand "Calendar API v3" and select `https://www.googleapis.com/auth/calendar.readonly`. Authorize, then exchange the auth code for tokens. Copy the `refresh_token`.
4. Paste into "Google credentials (JSON)" as:
   ```json
   {"refresh_token":"…","client_id":"…","client_secret":"…"}
   ```

## Refresh cadence

- Heart rate: every 30s (and on every new sample event from the Health service).
- Now Playing: instant — the Android companion pushes on every MediaSession metadata change.
- Weather + calendar: every 15 minutes (driven by the watch ticking and requesting a refresh).

The watch can also request an immediate refresh on tap by extending `request_refresh()` in `main.c`.

## Files of note

| Path                          | Purpose                              |
|-------------------------------|--------------------------------------|
| `package.json`                | Pebble project manifest              |
| `wscript`                     | waf build script                     |
| `src/c/main.c`                | Watchface rendering + AppMessage     |
| `src/pkjs/index.js`           | Phone-side data fetching             |
| `src/pkjs/config.json`        | Clay settings UI                     |
| `scripts/pebble`              | Docker wrapper for `pebble` CLI      |
| `build/project.pbw`           | Build output (sideload to watch)     |

## Known limitations

- Now Playing requires the **Glance NP Android app** to be installed AND the **Pebble (or Rebble) Android companion app** to be running — the latter is what bridges PebbleKit-sent data to the watch over Bluetooth.
- Some apps don't publish album art in their MediaSession (rare in 2026, but possible) — you'll see title/artist but no thumbnail.
- Google refresh tokens granted to "Testing" OAuth apps expire in 7 days unless the consent screen is published. Either publish (internal use is fine) or refresh manually.
- The Pebble emulator works in the Docker image but display rendering for `emery` is partial; testing on real hardware is recommended.
- If you change `messageKeys` order in `package.json`, the integer IDs in `android/app/src/main/java/.../MediaListenerService.java` (KEY_NOW_TITLE, KEY_NOW_ARTIST, KEY_ART_*) must be updated to match `build/js/message_keys.json`.

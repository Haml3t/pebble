# Glance — Privacy Policy

_Last updated: 2026-05-22_

Glance is a personal watchface for the Pebble Time 2 smartwatch. This document describes what data Glance accesses, how it is used, and where it is stored.

## Who maintains this app

Glance is a single-developer personal-use project. Source: <https://github.com/Haml3t/pebble>. Contact: open an issue at <https://github.com/Haml3t/pebble/issues>.

## What data Glance accesses

Glance reads the following Google account data on the user's behalf, using OAuth 2.0:

- **Calendar list** — the list of calendars the user has access to (`/calendar/v3/users/me/calendarList`), filtered to those the user has marked visible.
- **Calendar events** — upcoming events from those calendars, looking ahead seven days (`/calendar/v3/calendars/{id}/events`).

Glance requests only the `https://www.googleapis.com/auth/calendar.readonly` scope. It does not create, modify, or delete any calendar data.

Glance also reads weather data from OpenWeatherMap using a user-supplied API key. No Google account data is involved in the weather fetch.

## How the data is used

Calendar event titles and start times are displayed on the user's own Pebble Time 2 watch via Bluetooth. Specifically, the watchface displays the next upcoming event across the user's visible calendars, formatted as a short time prefix and the event summary (e.g. `10:00a · 1-1 with Sam`).

No analytics, telemetry, or other data is collected about how the watchface is used.

## Where the data is stored

- The OAuth `refresh_token`, `client_id`, and `client_secret` are stored in the Pebble companion app's local JavaScript sandbox storage (`localStorage`) on the user's own phone, and are mirrored to the watch's persistent storage so they survive app reinstalls.
- Calendar event data fetched from Google is held in memory only on the phone and the watch. It is overwritten on each refresh (every minute while the watchface is active) and is not written to disk in any persistent form.

No data is sent to any server controlled by the developer. There is no backend.

## Who receives the data

Nobody other than the user. The data path is:

```
Google Calendar API  ─►  user's phone (Pebble companion app, JS sandbox)  ─►  user's Pebble Time 2 watch (Bluetooth)
```

The developer cannot read the user's calendar data and has no ability to do so.

## How long the data is retained

- Credentials remain in the Pebble companion app's `localStorage` until the user uninstalls Glance, clears the Pebble companion app's data, or revokes access at <https://myaccount.google.com/permissions>.
- Calendar event data is held in memory only and is replaced on each refresh.

## How to revoke access

At any time, the user can revoke Glance's access to their Google account at <https://myaccount.google.com/permissions>. After revocation, Glance will no longer be able to fetch calendar data, and the watchface's calendar slot will display an authentication-error message.

## Children

Glance is not directed at children under 13.

## Changes to this policy

Any changes to this policy will be committed to the source repository at <https://github.com/Haml3t/pebble>; the file's git history is the authoritative changelog.

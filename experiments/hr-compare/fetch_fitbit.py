#!/usr/bin/env python3
"""Fetch intraday heart rate data from the Google Health API.

(Filename is fetch_fitbit.py for historical reasons — the data is still your
Fitbit data, it's just routed through Google's new health-data API instead of
the soon-to-be-deprecated Fitbit Web API.)

Two modes:

* --bootstrap   one-time interactive setup that prompts for Client ID/Secret/
                refresh token (obtained via Google OAuth Playground — see
                README.md) and writes secrets.local.json (gitignored).

* default       reads secrets.local.json, refreshes the access token, and
                pulls intraday HR for the given --date, writing a CSV of
                (unix_ts, bpm) to --out.

API reference:
https://developers.google.com/health/reference/rest/v4/users.dataTypes.dataPoints/list
"""

import argparse
import csv
import datetime as dt
import json
import sys
from pathlib import Path

import requests

SCRIPT_DIR = Path(__file__).resolve().parent
SECRETS_PATH = SCRIPT_DIR / "secrets.local.json"
TOKEN_URL = "https://oauth2.googleapis.com/token"
API_URL = ("https://health.googleapis.com/v4/users/me/"
           "dataTypes/heart-rate/dataPoints")


def load_secrets() -> dict:
    if not SECRETS_PATH.exists():
        sys.exit(f"error: {SECRETS_PATH.name} not found. "
                 f"Run `python3 fetch_fitbit.py --bootstrap` first.")
    return json.loads(SECRETS_PATH.read_text())


def save_secrets(s: dict) -> None:
    SECRETS_PATH.write_text(json.dumps(s, indent=2) + "\n")
    SECRETS_PATH.chmod(0o600)


def bootstrap() -> int:
    print("Google Health API one-time setup. Paste the three values from the\n"
          "OAuth Playground (see README.md for the walkthrough).\n")
    client_id     = input("Client ID:     ").strip()
    client_secret = input("Client Secret: ").strip()
    refresh_token = input("Refresh token: ").strip()

    # Sanity check by doing a token refresh — fails loudly if any value is
    # wrong rather than failing the first real query later.
    r = requests.post(TOKEN_URL, data={
        "client_id": client_id,
        "client_secret": client_secret,
        "refresh_token": refresh_token,
        "grant_type": "refresh_token",
    }, timeout=30)
    if r.status_code != 200:
        sys.exit(f"token refresh failed: {r.status_code} {r.text}")

    save_secrets({
        "client_id": client_id,
        "client_secret": client_secret,
        "refresh_token": refresh_token,
    })
    print(f"\nSaved {SECRETS_PATH.name}. You're done with setup.")
    return 0


def access_token(s: dict) -> str:
    r = requests.post(TOKEN_URL, data={
        "client_id": s["client_id"],
        "client_secret": s["client_secret"],
        "refresh_token": s["refresh_token"],
        "grant_type": "refresh_token",
    }, timeout=30)
    if r.status_code != 200:
        sys.exit(f"token refresh failed: {r.status_code} {r.text}")
    return r.json()["access_token"]


def physical_time_to_unix(ts: str) -> int:
    # RFC3339, always UTC ("...Z" or with explicit offset).
    return int(dt.datetime.fromisoformat(ts.replace("Z", "+00:00"))
               .timestamp())


def fetch_intraday(date_str: str, out_path: Path) -> int:
    s = load_secrets()
    token = access_token(s)

    # Use civil_time so the date filter matches the user's local day without
    # timezone gymnastics. The response's physical_time field is UTC and is
    # what we convert to unix epoch for the CSV.
    next_day = (dt.date.fromisoformat(date_str)
                + dt.timedelta(days=1)).isoformat()
    filt = (f'heart_rate.sample_time.civil_time >= "{date_str}T00:00:00"'
            f' AND '
            f'heart_rate.sample_time.civil_time < "{next_day}T00:00:00"')

    rows: list[tuple[int, int]] = []
    page_token = None
    pages = 0
    while True:
        params = {"filter": filt, "pageSize": 10000}
        if page_token:
            params["pageToken"] = page_token
        r = requests.get(API_URL,
                         headers={"Authorization": f"Bearer {token}"},
                         params=params, timeout=60)
        if r.status_code != 200:
            sys.exit(f"intraday fetch failed: {r.status_code} {r.text}")
        body = r.json()
        for dp in body.get("dataPoints", []):
            hr = dp.get("heartRate", {})
            phys = hr.get("sampleTime", {}).get("physicalTime")
            bpm = hr.get("beatsPerMinute")  # comes back as a string
            if phys is None or bpm is None:
                continue
            rows.append((physical_time_to_unix(phys), int(bpm)))
        pages += 1
        page_token = body.get("nextPageToken")
        if not page_token:
            break

    if not rows:
        sys.exit("no heart rate data points returned — was the date right, "
                 "and was 1Hz HR actually recorded (workout mode)?")

    rows.sort(key=lambda r: r[0])
    with out_path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["unix_ts", "bpm"])
        w.writerows(rows)

    print(f"wrote {len(rows)} rows to {out_path} ({pages} page(s))")
    print(f"span: {rows[0][0]}..{rows[-1][0]} "
          f"({rows[-1][0] - rows[0][0]}s)")
    return 0


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bootstrap", action="store_true",
                    help="one-time interactive secret setup")
    ap.add_argument("--date", help="YYYY-MM-DD in your local timezone")
    ap.add_argument("--out", default="fitbit.csv",
                    help="output CSV path (default fitbit.csv)")
    args = ap.parse_args()

    if args.bootstrap:
        raise SystemExit(bootstrap())
    if not args.date:
        ap.error("--date is required (or use --bootstrap)")
    out = Path(args.out)
    if not out.is_absolute():
        out = SCRIPT_DIR / out
    raise SystemExit(fetch_intraday(args.date, out))

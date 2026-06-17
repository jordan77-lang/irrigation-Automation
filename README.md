# ESP32-S3 PD-Stepper Irrigation

Battery-friendly firmware for flood irrigation: set open/close times on a website, leave the device on the port, and it wakes only to sync and move the valve.

## Architecture (no Supabase, no Netlify)

| Layer | What | Hosting |
|---|---|---|
| Schedule editor | Web UI to set open/close times | **GitHub Pages** (free) |
| Schedule storage | `schedules/schedules.json` + HMAC signature | **GitHub** (raw URLs) |
| Signing | GitHub Action with `SIGN_KEY` secret | **GitHub Actions** |
| Field device | ESP32 fetches schedule, deep sleeps, runs stepper | Your hardware |

You do **not** need Supabase (no database) or Netlify (GitHub Pages hosts the static editor). Add those later only if you want user accounts, a dashboard, or device status telemetry.

## Quick start

**Full checklists:** [SETUP.md](SETUP.md) (GitHub + site, no board) · [FLASHING.md](FLASHING.md) (when you have the board)

### 1. GitHub repo setup

1. Push this repo to GitHub.
2. **Settings → Secrets → Actions** → add `SIGN_KEY` (any long random string).
3. **Settings → Pages** → Source: **GitHub Actions**.
4. Push to `main` — the Pages workflow deploys `docs/` as your schedule editor.

### 2. Set a schedule

1. Open the GitHub Pages URL (e.g. `https://<user>.github.io/irrigation-Automation/`).
2. Add irrigation windows (open time, close time, weekly repeat).
3. Download `manual_events.json`.
4. Commit it to `schedules/manual_events.json` in the repo.
5. GitHub Actions signs and publishes `schedules/schedules.json`.

### 3. Flash the device

```bash
cp include/config.example.h include/config.h
# Edit config.h: Wi-Fi, DEVICE_ID, SIGN_KEY (must match GitHub secret), URLs

pio run -t upload
```

### 4. Deploy in the field

Connect the PD-Stepper to Wi-Fi (field hotspot or router), attach battery, leave on the port. The device:

1. Wakes briefly to sync NTP and fetch the signed schedule
2. Deep sleeps until ~1 minute before the next event
3. Opens or closes the valve at the scheduled time
4. Sleeps again

## How timekeeping works without draining the battery

- NTP sets the clock when Wi-Fi is on (seconds, not hours).
- The ESP32 RTC keeps time during **deep sleep** (~10–150 µA).
- The device computes seconds until the next event and sleeps until then.
- It wakes early, fine-tunes time, waits for the exact second, then moves the stepper.

## File layout

```
include/config.example.h   # Copy to config.h (gitignored)
src/main.cpp               # Battery-friendly firmware
schedules/manual_events.json
schedules/schedules.json   # Generated + signed by CI
devices/devices.json
scripts/generate_schedules.py
docs/index.html            # Schedule editor (GitHub Pages)
.github/workflows/
```

## Device config

Edit `include/config.h`:

- `WIFI_SSID` / `WIFI_PASS` — field network
- `DEVICE_ID` — must match an entry in the schedule JSON
- `SIGN_KEY` — must match the GitHub Actions secret
- `SCHEDULE_URL` / `SIGNATURE_URL` — raw GitHub URLs for your repo

## Schedule format

Each device has open/close events:

```json
{
  "id": "pd01-20260620T130000-open",
  "action": "open",
  "time": "2026-06-20T13:00:00Z",
  "virtual_angle": 1440.0,
  "expected_duration_s": 120
}
```

`virtual_angle` is absolute multi-turn position in degrees (1440 = 4 full turns).

## When you might add Supabase or Netlify later

- **Supabase** — multiple users, login, device status history, push notifications
- **Netlify** — alternative static hosting (same role as GitHub Pages) or serverless functions to commit schedules without using git manually

For a single-owner field setup, GitHub Pages + GitHub-hosted JSON is enough.

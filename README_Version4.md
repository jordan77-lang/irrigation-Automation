```markdown
# irrigation-Automation

This repository manages schedules for PD‑Stepper irrigation devices. It supports:
- A GitHub Action that produces or updates the schedule file `schedules/schedules.json`.
- HMAC-SHA256 signing of the schedule JSON (`schedules/schedules.json.sig`) using a single shared key stored in GitHub Actions secrets.
- A simple static schedule editor you can use locally to compose schedules and then commit/upload them through the GitHub web UI.

How it works (high level)
1. You (or the action) create/modify `schedules/manual_events.json` if you want exact times.
2. The Action runs (manually or nightly) and:
   - If `schedules/manual_events.json` exists it uses that as authoritative.
   - Otherwise it generates randomized bi-weekly open/close events from `devices/devices.json`.
   - It writes `schedules/schedules.json` and `schedules/schedules.json.sig` (HMAC).
   - Commits those files to the repo so devices can fetch from raw.githubusercontent.com or GitHub Pages.

Device requirements and expectations
- Each device is pre‑provisioned with:
  - device_id (e.g., "pd01")
  - the shared SIGN_KEY (for HMAC validation)
  - a persistent `current_virtual_position` (degrees) to support multi‑turn valves
- Devices fetch both files:
  - https://raw.githubusercontent.com/<owner>/irrigation-Automation/main/schedules/schedules.json
  - https://raw.githubusercontent.com/<owner>/irrigation-Automation/main/schedules/schedules.json.sig
- Device validates HMAC before trusting schedule.
- The schedule uses `virtual_angle` expressed in degrees (absolute multi‑turn value, e.g. 1440.0 = 4 turns).

Onboarding quick checklist
1. Create the repo `irrigation-Automation` (public).
2. Add the files in this repo (see below).
3. In repo Settings → Secrets and variables → Actions add SIGN_KEY (value: secret string used for HMAC).
4. Optionally edit `devices/devices.json` to list your devices and their open_turns, min/max durations, base_virtual_deg.
5. Run the workflow manually (Actions → Generate and sign PD‑Stepper schedules) or wait for the daily run.
6. Devices pull and verify the payload before executing events.

If you want me to push these files to your repo I can try — I'll need a short-lived PAT or permission. Otherwise run the included script to create the zip locally.
```
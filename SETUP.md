# Setup checklist

Do these steps **now** (no board needed), then flash when you're home.

## Phase 1 — GitHub (do today)

- [ ] **Push this repo** to `main` (if not already pushed)
- [ ] **Add Actions secret** `SIGN_KEY`
  - Go to: https://github.com/jordan77-lang/irrigation-Automation/settings/secrets/actions
  - New repository secret → Name: `SIGN_KEY` → Value: a long random string (run `scripts/generate_sign_key.ps1`)
  - Save the same value — you'll paste it into `include/config.h` when flashing
- [ ] **Enable GitHub Pages**
  - Go to: https://github.com/jordan77-lang/irrigation-Automation/settings/pages
  - **Build and deployment → Source:** GitHub Actions
- [ ] **Run the workflows** (after push)
  - Actions tab → **Deploy schedule editor (GitHub Pages)** → Run workflow
  - Actions tab → **Generate and sign schedules** → Run workflow
- [ ] **Confirm the site loads**
  - https://jordan77-lang.github.io/irrigation-Automation/
- [ ] **Confirm schedule URLs work**
  - https://raw.githubusercontent.com/jordan77-lang/irrigation-Automation/main/schedules/schedules.json
  - https://raw.githubusercontent.com/jordan77-lang/irrigation-Automation/main/schedules/schedules.json.sig

## Phase 2 — Schedule (do today)

- [ ] Open the schedule editor (GitHub Pages URL above)
- [ ] Set device ID `pd01`, open/close times, weekly repeat
- [ ] Download `manual_events.json`
- [ ] Upload to the repo:
  - https://github.com/jordan77-lang/irrigation-Automation/edit/main/schedules/manual_events.json
  - Paste JSON → Commit
- [ ] Wait for **Generate and sign schedules** action to finish (updates `schedules.json` + `.sig`)

## Phase 3 — Firmware config (before flashing)

- [ ] Install [PlatformIO IDE](https://platformio.org/install/ide?install=vscode) (VS Code extension)
- [ ] Run setup script (creates local `include/config.h`):
  ```powershell
  cd irrigation-Automation
  .\scripts\setup_config.ps1
  ```
- [ ] Edit `include/config.h`:
  - `WIFI_SSID` / `WIFI_PASS` — Wi‑Fi at the field (or home Wi‑Fi for first test)
  - `SIGN_KEY` — must match GitHub Actions secret exactly
  - `DEVICE_ID` — must match schedule (`pd01`)
- [ ] Build without uploading (verifies code compiles):
  ```powershell
  pio run
  ```

## Phase 4 — Flash at home

See [FLASHING.md](FLASHING.md).

## Quick links

| What | URL |
|---|---|
| Schedule editor | https://jordan77-lang.github.io/irrigation-Automation/ |
| Repo | https://github.com/jordan77-lang/irrigation-Automation |
| Actions | https://github.com/jordan77-lang/irrigation-Automation/actions |
| Pages settings | https://github.com/jordan77-lang/irrigation-Automation/settings/pages |
| Secrets | https://github.com/jordan77-lang/irrigation-Automation/settings/secrets/actions |
| Edit schedule file | https://github.com/jordan77-lang/irrigation-Automation/edit/main/schedules/manual_events.json |

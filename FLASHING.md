# Flashing guide (when you have the board)

## What you need

- PD-Stepper board (ESP32-S3)
- USB cable
- PC with VS Code + PlatformIO
- `include/config.h` already filled in (see [SETUP.md](SETUP.md))

## 1. Connect the board

1. Plug the PD-Stepper into USB.
2. In Device Manager (Windows), note the COM port (e.g. `COM5`, `COM7`).
3. Update `platformio.ini` if needed:
   ```ini
   upload_port = COM7
   monitor_port = COM7
   ```
   Or omit `upload_port` and PlatformIO will auto-detect.

## 2. Build and upload

From the `irrigation-Automation` folder:

```powershell
pio run -t upload
```

First build downloads ESP32 toolchains and libraries — may take several minutes.

## 3. Serial monitor (verify boot)

```powershell
pio device monitor
```

Expected output (similar to):

```
Wake cause: 0
Connecting Wi-Fi...
Wi-Fi connected
Syncing NTP...
Time: ...
Schedule fetched and cached
Next event: pd01-...
Sleeping 12345 s
```

Press `Ctrl+C` to exit the monitor.

## 4. First-test checklist

- [ ] Wi‑Fi connects (SSID/password correct in `config.h`)
- [ ] NTP syncs (time looks correct in serial log)
- [ ] Schedule downloads and HMAC verifies (`SIGN_KEY` matches GitHub secret)
- [ ] Device finds next event and enters deep sleep
- [ ] For a quick move test: set an open event **2–3 minutes in the future** in `manual_events.json`, re-run the sign workflow, reset the board

## 5. Troubleshooting

| Problem | Fix |
|---|---|
| `Upload failed` / port not found | Check USB cable, driver, COM port in `platformio.ini` |
| `Wi-Fi timeout` | SSID/password, 2.4 GHz network (ESP32 does not use 5 GHz) |
| `HMAC verification failed` | `SIGN_KEY` in `config.h` must match GitHub Actions secret; re-run sign workflow |
| `No events for this device` | `DEVICE_ID` in `config.h` must match schedule JSON |
| `No schedule available` | Push signed `schedules.json`; device needs Wi‑Fi at least once to cache |
| Board resets in a loop | Open serial monitor to read the error before sleep |

## 6. Field deployment

1. Flash at home with test Wi‑Fi first.
2. Change `WIFI_SSID` / `WIFI_PASS` to field network if different — reflash.
3. Attach battery pack.
4. Mount on valve.
5. Confirm serial shows sleep until next event before sealing enclosure.

## 7. Optional: upload filesystem

The firmware formats LittleFS on first boot (`LittleFS.begin(true)`). No separate filesystem upload is required for normal use.

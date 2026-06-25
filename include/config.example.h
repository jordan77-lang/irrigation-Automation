#pragma once

// Copy this file to config.h and fill in your values.
// config.h is gitignored so secrets stay local.

#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"

#define DEVICE_ID "pd01"

// Must match the SIGN_KEY GitHub Actions secret used to sign schedules.
#define SIGN_KEY "your-shared-sign-key"

#define SCHEDULE_URL \
  "https://raw.githubusercontent.com/jordan77-lang/irrigation-Automation/main/schedules/schedules.json"
#define SIGNATURE_URL \
  "https://raw.githubusercontent.com/jordan77-lang/irrigation-Automation/main/schedules/schedules.json.sig"

// Optional: GitHub PAT with Contents write on this repo. When set, the device updates
// schedules/device_status.json after it downloads a signed schedule (powers the site tell).
// Use a fine-grained token scoped to this repo only. Leave empty to disable.
#define GITHUB_STATUS_TOKEN ""
#define GITHUB_REPO "jordan77-lang/irrigation-Automation"
#define DEVICE_STATUS_PATH "schedules/device_status.json"

// PD-Stepper board pinout (Things by Josh)
#define PIN_STEP 5
#define PIN_DIR 6
#define PIN_EN 21
#define PIN_MS1 1
#define PIN_MS2 2
#define PIN_SPREAD 7
#define PIN_PG 15
#define PIN_CFG1 38
#define PIN_CFG2 48
#define PIN_CFG3 47
#define PIN_SDA 8
#define PIN_SCL 9
#define PIN_LED1 10
#define PIN_TMC_RX 18
#define PIN_TMC_TX 17
#define PIN_DIAG 16
#define PIN_VBUS 4

// TMC2209 run current as percent of max (official web UI default is 30; use 80+ for valve loads)
#define TMC_RUN_CURRENT_PERCENT 80

// Valve travel (multi-turn virtual angle in degrees)
#define CLOSED_VIRTUAL_ANGLE 0.0f
#define OPEN_VIRTUAL_ANGLE 1440.0f

// Stepper: 200 full steps/rev with 64x microstepping (MS1/MS2 on PD-Stepper)
#define STEPS_PER_REV 200
#define MICROSTEPS 64

// Timing / power
#define WAKE_EARLY_SEC 60          // wake before event for NTP fine-tune
#define GRACE_PERIOD_SEC 300       // still run if up to 5 min late
#define NTP_RESYNC_INTERVAL_SEC (24 * 3600)
#define SCHEDULE_REFRESH_INTERVAL_SEC (12 * 3600)
#define MOVE_TIMEOUT_MS 120000
#define WIFI_CONNECT_TIMEOUT_MS 20000
#define PG_WAIT_MS 10000          // wait up to 10s for PD power-good before moving
#define REFRESH_SCHEDULE_EVERY_WAKE 1
// 1 = stay awake between events (required for most USB wall warts)
#define USE_AWAKE_WAIT 1
// 1 = light sleep; 0 = deep sleep — only used when USE_AWAKE_WAIT is 0
#define USE_LIGHT_SLEEP 1
#define USB_SERIAL_WAIT_MS 500

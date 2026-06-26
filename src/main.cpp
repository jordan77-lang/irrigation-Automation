// ESP32-S3 PD-Stepper — battery-friendly scheduled irrigation firmware
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <esp_sleep.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#include <time.h>
#include <TMC2209.h>

#if __has_include("config.h")
#include "config.h"
#else
#include "config.example.h"
#warning "Using config.example.h — copy include/config.example.h to include/config.h"
#endif

#ifndef PG_WAIT_MS
#define PG_WAIT_MS 10000
#endif
#ifndef REFRESH_SCHEDULE_EVERY_WAKE
#define REFRESH_SCHEDULE_EVERY_WAKE 1
#endif
#ifndef PIN_MS1
#define PIN_MS1 1
#define PIN_MS2 2
#define PIN_SPREAD 7
#define PIN_CFG1 38
#define PIN_CFG2 48
#define PIN_CFG3 47
#endif
#ifndef PIN_LED1
#define PIN_LED1 10
#endif
#ifndef USE_LIGHT_SLEEP
#define USE_LIGHT_SLEEP 0
#endif
#ifndef USE_AWAKE_WAIT
#define USE_AWAKE_WAIT 0
#endif
#ifndef POWER_PROFILE
#if USE_AWAKE_WAIT
#define POWER_PROFILE 2
#elif USE_LIGHT_SLEEP
#define POWER_PROFILE 1
#else
#define POWER_PROFILE 0
#endif
#endif
#ifndef STEP_DELAY_START_US
#define STEP_DELAY_START_US 320
#endif
#ifndef STEP_DELAY_MIN_US
#define STEP_DELAY_MIN_US 150
#endif
#ifndef STEP_ACCEL_STEPS
#define STEP_ACCEL_STEPS 1200
#endif
#ifndef MOTION_RETRY_COUNT
#define MOTION_RETRY_COUNT 1
#endif
#ifndef ENABLE_DIAG_FAULT_CHECK
#define ENABLE_DIAG_FAULT_CHECK 1
#endif
#ifndef DIAG_ACTIVE_STATE
#define DIAG_ACTIVE_STATE HIGH
#endif
#ifndef USB_SERIAL_WAIT_MS
#define USB_SERIAL_WAIT_MS 500
#endif
#ifndef PIN_VBUS
#define PIN_VBUS 4
#endif
#ifndef PIN_TMC_RX
#define PIN_TMC_RX 18
#define PIN_TMC_TX 17
#endif
#ifndef PIN_DIAG
#define PIN_DIAG 16
#endif
#ifndef TMC_RUN_CURRENT_PERCENT
#define TMC_RUN_CURRENT_PERCENT 80
#endif
#ifndef GITHUB_STATUS_TOKEN
#define GITHUB_STATUS_TOKEN ""
#endif
#ifndef GITHUB_REPO
#define GITHUB_REPO "jordan77-lang/irrigation-Automation"
#endif
#ifndef DEVICE_STATUS_PATH
#define DEVICE_STATUS_PATH "schedules/device_status.json"
#endif

static TMC2209 stepper_driver;
static HardwareSerial& tmc_serial = Serial2;

static const char* PREFS_NS = "pdstepper";
static const char* SCHEDULE_CACHE_PATH = "/schedule.json";
Preferences prefs;
float current_virtual_position = CLOSED_VIRTUAL_ANGLE;
float degrees_per_microstep;

struct ScheduleEvent {
  bool valid = false;
  String id;
  String action;
  time_t timestamp = 0;
  float virtual_angle = 0;
  uint32_t expected_duration_s = 60;
};

static String runtime_state = "booting";
static String runtime_detail;
static String last_event_id;
static String last_event_action;
static String last_event_time;
static String last_move_result;
static String last_move_detail;
static int last_wakeup_cause = -1;

// ---------------------------------------------------------------------------
// Logging helpers
// ---------------------------------------------------------------------------

static void log_line(const char* msg) {
  Serial.println(msg);
}

static void log_f(const char* label, float v) {
  Serial.print(label);
  Serial.println(v);
}

static void set_runtime_status(const char* state, const String& detail = String()) {
  runtime_state = state ? state : "";
  runtime_detail = detail;
}

static void remember_event_result(const ScheduleEvent& event, bool ok, const String& detail = String()) {
  last_event_id = event.id;
  last_event_action = event.action;
  last_event_time = "";
  last_move_result = ok ? "ok" : "failed";
  last_move_detail = detail;
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

static time_t parse_iso8601_utc(const char* iso) {
  if (!iso || strlen(iso) < 19) return 0;

  struct tm tm = {};
  int year, mon, day, hour, min, sec;
  if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &year, &mon, &day, &hour, &min, &sec) != 6) {
    return 0;
  }
  tm.tm_year = year - 1900;
  tm.tm_mon = mon - 1;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = min;
  tm.tm_sec = sec;
  tm.tm_isdst = 0;

  setenv("TZ", "UTC0", 1);
  tzset();
  return mktime(&tm);
}

static void log_time(const char* label, time_t t) {
  char buf[32];
  struct tm tm;
  gmtime_r(&t, &tm);
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  Serial.print(label);
  Serial.println(buf);
}

static bool sync_ntp() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  log_line("Syncing NTP...");

  for (int i = 0; i < 20; i++) {
    time_t now = time(nullptr);
    if (now > 1700000000) {
      Serial.print("Time: ");
      Serial.println(ctime(&now));
      prefs.putULong("last_ntp", (uint32_t)now);
      return true;
    }
    delay(500);
  }
  return false;
}

static bool ntp_stale() {
  uint32_t last = prefs.getULong("last_ntp", 0);
  if (last == 0) return true;
  return (time(nullptr) - (time_t)last) > NTP_RESYNC_INTERVAL_SEC;
}

// ---------------------------------------------------------------------------
// Wi-Fi (brief connect, then off before sleep)
// ---------------------------------------------------------------------------

static bool connect_wifi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  log_line("Connecting Wi-Fi...");

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_CONNECT_TIMEOUT_MS) {
      log_line("Wi-Fi timeout");
      return false;
    }
    delay(250);
  }
  log_line("Wi-Fi connected");
  return true;
}

static void disconnect_wifi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// ---------------------------------------------------------------------------
// HMAC-SHA256
// ---------------------------------------------------------------------------

static bool hex_equals(const unsigned char* digest, const String& sig_hex) {
  char hex[65];
  for (int i = 0; i < 32; i++) {
    sprintf(hex + (i * 2), "%02x", digest[i]);
  }
  hex[64] = '\0';
  return sig_hex.equalsIgnoreCase(hex);
}

static bool verify_hmac(const String& json, const String& sig_hex, const String& key) {
  if (sig_hex.length() == 0 || key.length() == 0) return false;

  unsigned char digest[32];
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info) return false;

  int rc = mbedtls_md_hmac(
      info,
      reinterpret_cast<const unsigned char*>(key.c_str()),
      key.length(),
      reinterpret_cast<const unsigned char*>(json.c_str()),
      json.length(),
      digest);
  return rc == 0 && hex_equals(digest, sig_hex);
}

// ---------------------------------------------------------------------------
// Schedule cache (LittleFS)
// ---------------------------------------------------------------------------

static bool cache_schedule(const String& json) {
  File f = LittleFS.open(SCHEDULE_CACHE_PATH, "w");
  if (!f) return false;
  f.print(json);
  f.close();
  prefs.putULong("sched_cached", (uint32_t)time(nullptr));
  return true;
}

static String load_cached_schedule() {
  if (!LittleFS.exists(SCHEDULE_CACHE_PATH)) return "";
  File f = LittleFS.open(SCHEDULE_CACHE_PATH, "r");
  if (!f) return "";
  String json = f.readString();
  f.close();
  return json;
}

static bool schedule_cache_stale() {
  uint32_t cached = prefs.getULong("sched_cached", 0);
  if (cached == 0) return true;
  return (time(nullptr) - (time_t)cached) > SCHEDULE_REFRESH_INTERVAL_SEC;
}

static bool http_get_string(const char* url, String& out) {
  HTTPClient http;
  http.begin(url);
  http.setTimeout(15000);
  int code = http.GET();
  if (code == 200) {
    out = http.getString();
    http.end();
    return true;
  }
  http.end();
  return false;
}

static String iso_utc_now() {
  time_t now = time(nullptr);
  if (now <= 0) return "";
  struct tm tm;
  gmtime_r(&now, &tm);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return String(buf);
}

static String iso_utc(time_t ts) {
  if (ts <= 0) return "";
  struct tm tm;
  gmtime_r(&ts, &tm);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return String(buf);
}

static String base64_encode_str(const String& in) {
  size_t olen = 0;
  const unsigned char* src = reinterpret_cast<const unsigned char*>(in.c_str());
  size_t slen = in.length();
  mbedtls_base64_encode(nullptr, 0, &olen, src, slen);
  unsigned char* buf = (unsigned char*)malloc(olen + 1);
  if (!buf) return "";
  if (mbedtls_base64_encode(buf, olen, &olen, src, slen) != 0) {
    free(buf);
    return "";
  }
  String out(reinterpret_cast<char*>(buf), olen);
  free(buf);
  return out;
}

static String base64_decode_str(const String& in) {
  size_t olen = 0;
  const unsigned char* src = reinterpret_cast<const unsigned char*>(in.c_str());
  size_t slen = in.length();
  if (mbedtls_base64_decode(nullptr, 0, &olen, src, slen) != 0 || olen == 0) {
    return "";
  }
  unsigned char* buf = (unsigned char*)malloc(olen + 1);
  if (!buf) return "";
  if (mbedtls_base64_decode(buf, olen, &olen, src, slen) != 0) {
    free(buf);
    return "";
  }
  String out(reinterpret_cast<char*>(buf), olen);
  free(buf);
  return out;
}

static float read_vbus_volts();
static bool is_pg_good();

static bool status_report_enabled() {
  return GITHUB_STATUS_TOKEN[0] != '\0';
}

static bool report_device_status(const String* schedule_json = nullptr) {
  if (!status_report_enabled()) return false;

  String generated_at;
  String open_time;
  String close_time;
  if (schedule_json) {
    DynamicJsonDocument sched(max((size_t)4096, schedule_json->length() + 2048));
    if (deserializeJson(sched, *schedule_json)) {
      log_line("Status report: schedule JSON parse failed");
      return false;
    }

    generated_at = sched["generated_at"] | "";
    if (!generated_at.length()) {
      log_line("Status report: no generated_at");
      return false;
    }

    JsonArray events = sched["devices"][DEVICE_ID].as<JsonArray>();
    if (events.isNull()) {
      log_line("Status report: no events for device");
      return false;
    }

    for (JsonObject evt : events) {
      const char* action = evt["action"] | "";
      const char* when = evt["time"] | "";
      if (!when[0]) continue;
      if (strcmp(action, "open") == 0 && !open_time.length()) open_time = when;
      if (strcmp(action, "close") == 0 && !close_time.length()) close_time = when;
    }
  }

  String status_at = iso_utc_now();
  DynamicJsonDocument status_doc(4096);
  deserializeJson(status_doc, "{}");

  String api_url = String("https://api.github.com/repos/") + GITHUB_REPO + "/contents/" + DEVICE_STATUS_PATH;
  String sha;
  {
    HTTPClient http;
    http.begin(api_url);
    http.setTimeout(15000);
    http.addHeader("Authorization", String("Bearer ") + GITHUB_STATUS_TOKEN);
    http.addHeader("Accept", "application/vnd.github+json");
    http.addHeader("X-GitHub-Api-Version", "2022-11-28");
    int code = http.GET();
    if (code == 200) {
      String body = http.getString();
      DynamicJsonDocument doc(4096);
      if (!deserializeJson(doc, body)) {
        sha = doc["sha"] | "";
        String content_b64 = doc["content"] | "";
        content_b64.replace("\n", "");
        String existing = base64_decode_str(content_b64);
        if (existing.length()) {
          DynamicJsonDocument existing_doc(4096);
          if (!deserializeJson(existing_doc, existing)) {
            status_doc = existing_doc;
          }
        }
      }
    }
    http.end();
  }

  JsonObject root = status_doc.as<JsonObject>();
  if (root.isNull()) root = status_doc.to<JsonObject>();
  JsonObject dev = root[DEVICE_ID].is<JsonObject>()
      ? root[DEVICE_ID].as<JsonObject>()
      : root.createNestedObject(DEVICE_ID);
  if (generated_at.length()) dev["schedule_generated_at"] = generated_at;
  if (status_at.length()) dev["received_at"] = status_at;
  if (open_time.length()) dev["open_time"] = open_time;
  if (close_time.length()) dev["close_time"] = close_time;
  dev["state"] = runtime_state;
  dev["detail"] = runtime_detail;
  dev["power_profile"] = POWER_PROFILE;
  dev["pg_good"] = is_pg_good();
  dev["vbus_volts"] = read_vbus_volts();
  dev["virtual_position_deg"] = current_virtual_position;
  dev["uptime_s"] = millis() / 1000UL;
  dev["wake_cause"] = last_wakeup_cause;
  if (WiFi.status() == WL_CONNECTED) {
    dev["wifi_rssi_dbm"] = WiFi.RSSI();
  }
  if (last_event_id.length()) dev["last_event_id"] = last_event_id;
  if (last_event_action.length()) dev["last_event_action"] = last_event_action;
  if (last_event_time.length()) dev["last_event_time"] = last_event_time;
  if (last_move_result.length()) dev["last_move_result"] = last_move_result;
  if (last_move_detail.length()) dev["last_move_detail"] = last_move_detail;

  String status_json;
  serializeJson(status_doc, status_json);

  String b64 = base64_encode_str(status_json);
  if (!b64.length()) {
    log_line("Status report: base64 encode failed");
    return false;
  }

  String put_body = String("{\"message\":\"device ") + DEVICE_ID + ": schedule received\","
    + "\"content\":\"" + b64 + "\"";
  if (sha.length()) put_body += ",\"sha\":\"" + sha + "\"";
  put_body += "}";

  for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt > 0) {
      Serial.print("Status report retry ");
      Serial.println(attempt + 1);
      delay(2000);
      // Re-fetch sha in case the file changed between attempts
      sha = "";
      HTTPClient get_http;
      get_http.begin(api_url);
      get_http.setTimeout(15000);
      get_http.addHeader("Authorization", String("Bearer ") + GITHUB_STATUS_TOKEN);
      get_http.addHeader("Accept", "application/vnd.github+json");
      get_http.addHeader("X-GitHub-Api-Version", "2022-11-28");
      if (get_http.GET() == 200) {
        String body = get_http.getString();
        StaticJsonDocument<512> doc;
        if (!deserializeJson(doc, body)) sha = doc["sha"] | "";
      }
      get_http.end();
      put_body = String("{\"message\":\"device ") + DEVICE_ID + ": schedule received\","
        + "\"content\":\"" + b64 + "\"";
      if (sha.length()) put_body += ",\"sha\":\"" + sha + "\"";
      put_body += "}";
    }

    HTTPClient http;
    http.begin(api_url);
    http.setTimeout(20000);
    http.addHeader("Authorization", String("Bearer ") + GITHUB_STATUS_TOKEN);
    http.addHeader("Accept", "application/vnd.github+json");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-GitHub-Api-Version", "2022-11-28");
    int code = http.PUT(put_body);
    http.end();

    if (code == 200 || code == 201) {
      if (generated_at.length()) prefs.putString("sched_ack_gen", generated_at);
      log_line("Status report: device status published");
      return true;
    }
    Serial.print("Status report HTTP ");
    Serial.println(code);
  }
  return false;
}

static String schedule_generated_at(const String& json) {
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, json)) return "";
  return String(doc["generated_at"] | "");
}

static bool fetch_and_cache_schedule() {
  String json;
  String sig;
  if (!http_get_string(SCHEDULE_URL, json)) {
    log_line("Schedule download failed");
    return false;
  }
  if (!http_get_string(SIGNATURE_URL, sig)) {
    log_line("Signature download failed");
    return false;
  }
  sig.trim();
  if (!verify_hmac(json, sig, SIGN_KEY)) {
    log_line("HMAC verification failed");
    return false;
  }
  if (!cache_schedule(json)) {
    log_line("Failed to cache schedule");
    return false;
  }
  String new_gen = schedule_generated_at(json);
  String prev_gen = prefs.getString("sched_gen", "");
  if (new_gen.length() && new_gen != prev_gen) {
    prefs.remove("last_event_id");
    prefs.putString("sched_gen", new_gen);
    log_line("New schedule version — reset event progress");
  }

  log_line("Schedule fetched and cached");
  Serial.print("Schedule generated_at: ");
  Serial.println(schedule_generated_at(json));
  Serial.print("Device fetch time: ");
  Serial.println(iso_utc_now());
  set_runtime_status("schedule-synced", "Signed schedule cached");
  if (!report_device_status(&json)) {
    log_line("Status report failed — site may not show sync yet");
  }
  return true;
}

static bool fetch_schedule_with_retries(int attempts = 3) {
  for (int i = 0; i < attempts; i++) {
    if (i > 0) {
      Serial.print("Schedule fetch retry ");
      Serial.println(i + 1);
      delay(3000);
    }
    if (fetch_and_cache_schedule()) return true;
  }
  return false;
}

static bool load_schedule_json(String& json) {
#if REFRESH_SCHEDULE_EVERY_WAKE
  if (connect_wifi()) {
    fetch_schedule_with_retries();
    disconnect_wifi();
  }
#else
  if (schedule_cache_stale() || !LittleFS.exists(SCHEDULE_CACHE_PATH)) {
    if (connect_wifi()) {
      fetch_schedule_with_retries();
      disconnect_wifi();
    }
  }
#endif
  json = load_cached_schedule();
  return json.length() > 0;
}

// ---------------------------------------------------------------------------
// Event selection
// ---------------------------------------------------------------------------

static bool event_already_done(const String& id) {
  String last = prefs.getString("last_event_id", "");
  return last == id;
}

static void mark_event_done(const String& id) {
  prefs.putString("last_event_id", id);
}

static ScheduleEvent find_next_event(const String& json) {
  ScheduleEvent best;
  DynamicJsonDocument doc(max((size_t)4096, json.length() + 2048));
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    log_line("JSON parse error");
    return best;
  }

  JsonArray events = doc["devices"][DEVICE_ID].as<JsonArray>();
  if (events.isNull()) {
    log_line("No events for this device");
    return best;
  }

  time_t now = time(nullptr);

  for (JsonObject evt : events) {
    String id = evt["id"] | "";
    if (id.length() == 0 || event_already_done(id)) continue;

    const char* time_str = evt["time"];
    time_t ts = parse_iso8601_utc(time_str);
    if (ts == 0) continue;

    // Skip events too far in the past
    if (ts + GRACE_PERIOD_SEC < now) {
      mark_event_done(id);
      continue;
    }

    if (!best.valid || ts < best.timestamp) {
      best.valid = true;
      best.id = id;
      best.action = evt["action"] | "";
      best.timestamp = ts;
      best.virtual_angle = evt["virtual_angle"] | CLOSED_VIRTUAL_ANGLE;
      best.expected_duration_s = evt["expected_duration_s"] | 60;
    }
  }

  return best;
}

// ---------------------------------------------------------------------------
// Hardware
// ---------------------------------------------------------------------------

static void configure_pd_12v();
static void led_sos();

// PD-Stepper CH224K: PG active LOW = negotiated voltage matches request (motor rail on)
static float read_vbus_volts() {
  uint32_t mv_sum = 0;
  for (int i = 0; i < 10; i++) {
    mv_sum += analogReadMilliVolts(PIN_VBUS);
    delayMicroseconds(100);
  }
  const float div_ratio = 0.1189427313f;
  return (mv_sum / 10.0f / 1000.0f) / div_ratio;
}

static bool is_pg_good() {
  return digitalRead(PIN_PG) == LOW;
}

static void log_power_state(const char* label) {
  Serial.print(label);
  Serial.print(" PG=");
  Serial.print(digitalRead(PIN_PG));
  Serial.print(" VBUS=");
  Serial.print(read_vbus_volts(), 1);
  Serial.println("V");
}

static void early_pd_init() {
  pinMode(PIN_CFG1, OUTPUT);
  pinMode(PIN_CFG2, OUTPUT);
  pinMode(PIN_CFG3, OUTPUT);
  configure_pd_12v();
}

static float read_as5600_degrees() {
  Wire.beginTransmission(0x36);
  Wire.write(0x0E);
  if (Wire.endTransmission() != 0) return -1.0f;
  if (Wire.requestFrom(0x36, 2) != 2) return -1.0f;
  uint16_t raw = (Wire.read() << 8) | Wire.read();
  return (raw & 0x0FFF) * 360.0f / 4096.0f;
}

static bool wait_power_good() {
  log_line("Waiting for PG (motor power)...");
  configure_pd_12v();
  delay(800);
  const int attempts = PG_WAIT_MS / 100;
  for (int i = 0; i < attempts; i++) {
    if (is_pg_good()) {
      Serial.print("PG ready (");
      Serial.print((i + 1) * 100);
      Serial.print(" ms, VBUS ");
      Serial.print(read_vbus_volts(), 1);
      Serial.println(" V)");
      return true;
    }
    delay(100);
  }
  log_line("PG not good — check USB-C PD charger and cable");
  log_power_state("Last check:");
  return false;
}

static void init_tmc2209() {
  pinMode(PIN_DIAG, INPUT);
  digitalWrite(PIN_EN, LOW);  // official PD-Stepper: EN held low; UART enable/disable

  stepper_driver.setup(tmc_serial, 115200, TMC2209::SERIAL_ADDRESS_0, PIN_TMC_RX, PIN_TMC_TX);
  stepper_driver.setRunCurrent(TMC_RUN_CURRENT_PERCENT);
  stepper_driver.setMicrostepsPerStep(MICROSTEPS);
  stepper_driver.enableAutomaticCurrentScaling();
  stepper_driver.enableStealthChop();
  stepper_driver.setCoolStepDurationThreshold(5000);
  stepper_driver.disable();
}

static void motor_enable() {
  if (!is_pg_good()) return;
  stepper_driver.enable();  // UART only — EN pin stays LOW always per Josh's design
}

static void motor_disable() {
  stepper_driver.disable();  // UART only — never raise EN HIGH, it disrupts UART comms
}

static void led_blink(int times, int on_ms = 120, int off_ms = 120) {
  pinMode(PIN_LED1, OUTPUT);
  for (int i = 0; i < times; i++) {
    digitalWrite(PIN_LED1, HIGH);
    delay(on_ms);
    digitalWrite(PIN_LED1, LOW);
    delay(off_ms);
  }
}

// ... dot dot dot — dash dash dash — dot dot dot
static void led_sos() {
  pinMode(PIN_LED1, OUTPUT);
  const int dot = 150, dash = 450, gap = 150, letter_gap = 400, repeat_gap = 1000;
  for (int repeat = 0; repeat < 3; repeat++) {
    // S: 3 dots
    for (int i = 0; i < 3; i++) { digitalWrite(PIN_LED1, HIGH); delay(dot); digitalWrite(PIN_LED1, LOW); delay(gap); }
    delay(letter_gap);
    // O: 3 dashes
    for (int i = 0; i < 3; i++) { digitalWrite(PIN_LED1, HIGH); delay(dash); digitalWrite(PIN_LED1, LOW); delay(gap); }
    delay(letter_gap);
    // S: 3 dots
    for (int i = 0; i < 3; i++) { digitalWrite(PIN_LED1, HIGH); delay(dot); digitalWrite(PIN_LED1, LOW); delay(gap); }
    delay(repeat_gap);
  }
}

static void configure_pd_12v() {
  digitalWrite(PIN_CFG1, LOW);
  digitalWrite(PIN_CFG2, LOW);
  digitalWrite(PIN_CFG3, HIGH);
}

static void init_hardware() {
  early_pd_init();
  pinMode(PIN_PG, INPUT);
  pinMode(PIN_VBUS, INPUT);
  analogSetPinAttenuation(PIN_VBUS, ADC_11db);
  pinMode(PIN_MS1, OUTPUT);
  pinMode(PIN_MS2, OUTPUT);
  pinMode(PIN_SPREAD, OUTPUT);
  digitalWrite(PIN_MS1, LOW);
  digitalWrite(PIN_MS2, LOW);
  digitalWrite(PIN_SPREAD, LOW);

  Wire.begin(PIN_SDA, PIN_SCL);
  pinMode(PIN_STEP, OUTPUT);
  pinMode(PIN_DIR, OUTPUT);
  pinMode(PIN_EN, OUTPUT);
  digitalWrite(PIN_STEP, LOW);
  digitalWrite(PIN_DIR, LOW);

  init_tmc2209();
  degrees_per_microstep = 360.0f / (STEPS_PER_REV * MICROSTEPS);

  delay(500);
}

static float angular_distance_deg(float a, float b) {
  float d = fabsf(a - b);
  if (d > 180.0f) d = 360.0f - d;
  return d;
}

static bool valve_encoder_looks_closed() {
  float enc = read_as5600_degrees();
  if (enc < 0.0f) return false;
  float enc_closed = prefs.getFloat("enc_at_close", -1.0f);
  if (enc_closed < 0.0f) return false;
  return angular_distance_deg(enc, enc_closed) < 20.0f;
}

// Virtual position can say "open" after a prior run while the valve is physically closed.
static bool reconcile_virtual_before_open(float target) {
  if (target <= CLOSED_VIRTUAL_ANGLE + 180.0f) return false;

  float enc_closed = prefs.getFloat("enc_at_close", -1.0f);
  if (enc_closed < 0.0f) {
    log_line("Open: no encoder baseline — moving from closed");
    current_virtual_position = CLOSED_VIRTUAL_ANGLE;
    return true;
  }
  if (valve_encoder_looks_closed()) {
    log_line("Open: encoder looks closed — moving despite virtual position");
    current_virtual_position = CLOSED_VIRTUAL_ANGLE;
    return true;
  }
  return false;
}

static bool move_to_virtual_angle(float target, uint32_t timeout_ms) {
  disconnect_wifi();

  if (!wait_power_good()) {
    last_move_detail = "power-pd-not-ready";
    log_line("Power not good — skipping move");
    motor_disable();
    return false;
  }

  float delta = target - current_virtual_position;
  if (fabs(delta) < degrees_per_microstep) {
    if (target > CLOSED_VIRTUAL_ANGLE + 180.0f && reconcile_virtual_before_open(target)) {
      delta = target - current_virtual_position;
    } else {
      last_move_detail = "already-at-target";
      Serial.println("Already at target — no steps needed");
      led_blink(1, 400, 0);
      return true;
    }
  }

  float enc_before = read_as5600_degrees();
  int direction = delta > 0 ? 1 : -1;
  const long total_steps = labs((long)(delta / degrees_per_microstep));
  if (total_steps <= 0) {
    last_move_detail = "no-steps-after-quantization";
    log_line("No move steps after quantization");
    return true;
  }
  const bool multi_turn = fabs(delta) >= 360.0f;
  Serial.print("Steps to move: ");
  Serial.println(total_steps);

  for (int attempt = 0; attempt <= MOTION_RETRY_COUNT; attempt++) {
    // Re-apply TMC settings in case a power glitch reset the driver over UART
    stepper_driver.setRunCurrent(TMC_RUN_CURRENT_PERCENT);
    stepper_driver.setMicrostepsPerStep(MICROSTEPS);
    stepper_driver.enableAutomaticCurrentScaling();
    stepper_driver.enableStealthChop();

    digitalWrite(PIN_DIR, direction > 0 ? HIGH : LOW);
    motor_enable();
    if (stepper_driver.hardwareDisabled()) {
      last_move_detail = "tmc-enable-failed";
      log_line("TMC2209 still disabled after enable");
      motor_disable();
      return false;
    }

    uint32_t start = millis();
    long steps_remaining = total_steps;
    long steps_moved = 0;
    bool fail_timeout = false;
    bool fail_pg = false;
    bool fail_diag = false;
    int diag_hits = 0;

    uint32_t start_delay_us = STEP_DELAY_START_US + (uint32_t)(attempt * 80);
    uint32_t min_delay_us = STEP_DELAY_MIN_US + (uint32_t)(attempt * 30);
    if (start_delay_us < min_delay_us) start_delay_us = min_delay_us;
    long accel_steps = STEP_ACCEL_STEPS;
    if (accel_steps < 1) accel_steps = 1;

    while (steps_remaining > 0) {
      if (millis() - start > timeout_ms) {
        fail_timeout = true;
        break;
      }
      if (!is_pg_good()) {
        fail_pg = true;
        break;
      }

#if ENABLE_DIAG_FAULT_CHECK
      if (steps_moved > 200) {
        if (digitalRead(PIN_DIAG) == DIAG_ACTIVE_STATE) diag_hits++;
        else diag_hits = 0;
        if (diag_hits >= 3) {
          fail_diag = true;
          break;
        }
      }
#endif

      long edge_steps = steps_moved < (total_steps - steps_moved)
          ? steps_moved
          : (total_steps - steps_moved);
      if (edge_steps > accel_steps) edge_steps = accel_steps;
      uint32_t step_delay_us = start_delay_us;
      if (edge_steps > 0 && start_delay_us > min_delay_us) {
        step_delay_us = start_delay_us - (uint32_t)((start_delay_us - min_delay_us) * edge_steps / accel_steps);
      }

      digitalWrite(PIN_STEP, HIGH);
      delayMicroseconds(step_delay_us);
      digitalWrite(PIN_STEP, LOW);
      delayMicroseconds(step_delay_us);
      steps_remaining--;
      steps_moved++;
    }

    motor_disable();

    if (!fail_timeout && !fail_pg && !fail_diag) {
      float enc_after = read_as5600_degrees();
      log_f("Encoder deg before: ", enc_before);
      log_f("Encoder deg after: ", enc_after);
      if (!multi_turn && enc_before >= 0.0f && enc_after >= 0.0f && fabs(enc_after - enc_before) < 1.0f) {
        last_move_detail = "encoder-no-motion";
        log_line("Encoder did not move — treating as failure");
      } else {
        last_move_detail = multi_turn ? "move-complete-multi-turn" : "move-complete";
        current_virtual_position = target;
        prefs.putFloat("virtual_pos", current_virtual_position);
        if (fabs(target - CLOSED_VIRTUAL_ANGLE) < degrees_per_microstep && enc_after >= 0.0f) {
          prefs.putFloat("enc_at_close", enc_after);
          log_f("Saved closed encoder baseline: ", enc_after);
        }
        return true;
      }
    } else {
      if (fail_timeout) {
        last_move_detail = "move-timeout";
        log_line("Move timeout");
      }
      if (fail_pg) {
        last_move_detail = "pg-lost-during-move";
        log_line("PG lost during move");
        log_power_state("PG lost:");
      }
      if (fail_diag) {
        last_move_detail = "diag-fault";
        log_line("DIAG fault asserted during move");
      }
    }

    if (attempt < MOTION_RETRY_COUNT) {
      Serial.print("Move retry ");
      Serial.print(attempt + 1);
      Serial.println(" with slower ramp");
      delay(300);
      if (!wait_power_good()) {
        last_move_detail = "power-pd-not-ready-retry";
        log_line("Power not good before retry");
        break;
      }
      continue;
    }
  }

  if (!last_move_detail.length()) last_move_detail = "move-failed";
  led_sos();
  return false;
}

// ---------------------------------------------------------------------------
// Wait / sleep — awake wait keeps USB wall warts from shutting off
// ---------------------------------------------------------------------------

static void board_sleep_seconds(uint64_t seconds) {
  if (seconds < 1) seconds = 1;
  Serial.print("Waiting ");
  Serial.print((unsigned long)seconds);
  Serial.flush();
  disconnect_wifi();

  if (POWER_PROFILE == 2) {
    Serial.println(" s (awake profile)");
    Serial.flush();
    const uint32_t start = millis();
    const uint32_t duration_ms = (uint32_t)(seconds * 1000ULL);
    uint32_t last_blink = 0;
    while (millis() - start < duration_ms) {
      if (millis() - last_blink > 30000) {
        led_blink(1, 80, 0);
        last_blink = millis();
      }
      delay(200);
    }
    return;
  }

  if (POWER_PROFILE == 1) {
    Serial.println(" s (light sleep profile)");
    Serial.flush();
    esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);
    esp_light_sleep_start();
    return;
  }

  Serial.println(" s (deep sleep profile)");
  Serial.flush();
  esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);
  esp_deep_sleep_start();
}

static bool wait_for_upcoming_events(String& schedule_json, ScheduleEvent& next) {
  for (int i = 0; i < 20; i++) {
    next = find_next_event(schedule_json);
    if (next.valid) return true;

    log_line("No upcoming events — retrying schedule fetch in 30s");
    board_sleep_seconds(30);

    if (connect_wifi()) {
      fetch_schedule_with_retries();
      disconnect_wifi();
    }
    schedule_json = load_cached_schedule();
    if (!schedule_json.length()) break;
  }
  return false;
}

static void board_restart_after(uint64_t seconds) {
  board_sleep_seconds(seconds);
  esp_restart();
}

static void wait_until(time_t target) {
  while (time(nullptr) < target) {
    delay(200);
  }
}

static void sleep_until_event(time_t event_time) {
  time_t now = time(nullptr);
  int64_t wake_at = event_time - WAKE_EARLY_SEC;
  int64_t delta = wake_at - now;
  if (delta > 2) {
    board_sleep_seconds((uint64_t)delta);
  }
}

// ---------------------------------------------------------------------------
// Main flow — setup() runs each wake cycle; loop() is unused
// ---------------------------------------------------------------------------

void setup() {
  early_pd_init();
  delay(500);

  Serial.begin(115200);
  delay(USB_SERIAL_WAIT_MS);

  init_hardware();
  led_blink(2);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  last_wakeup_cause = (int)cause;
  Serial.print("Wake cause: ");
  Serial.println((int)cause);
  if (cause == ESP_SLEEP_WAKEUP_UNDEFINED) {
    log_line("Power-on boot — syncing time and fetching schedule");
  }
  set_runtime_status("booting", cause == ESP_SLEEP_WAKEUP_UNDEFINED ? "Power-on boot" : "Wake cycle");

  prefs.begin(PREFS_NS, false);
  current_virtual_position = prefs.getFloat("virtual_pos", CLOSED_VIRTUAL_ANGLE);

  log_f("Virtual position: ", current_virtual_position);

  if (!LittleFS.begin(true)) {
    log_line("LittleFS mount failed");
  }

  if (connect_wifi()) {
    set_runtime_status("online", "Wi-Fi connected for NTP");
    sync_ntp();
    disconnect_wifi();
  } else {
    set_runtime_status("offline", "Wi-Fi connect failed");
    log_line("Wi-Fi failed — using cached schedule if available");
  }

  if (time(nullptr) < 1700000000) {
    log_line("Invalid time after NTP — retrying");
    if (connect_wifi()) {
      sync_ntp();
      disconnect_wifi();
    }
    if (time(nullptr) < 1700000000) {
      board_restart_after(300);
    }
  }

  log_time("Now: ", time(nullptr));

  String schedule_json;
  if (!load_schedule_json(schedule_json)) {
    log_line("No schedule available");
    board_restart_after(3600);
  }

  ScheduleEvent next;
  if (!wait_for_upcoming_events(schedule_json, next)) {
    log_line("No upcoming events after retries — restart in 5 min");
    board_restart_after(300);
  }

  time_t now = time(nullptr);
  Serial.print("Next event: ");
  Serial.println(next.id);
  log_time("Event time: ", next.timestamp);
  set_runtime_status("waiting", String("Next ") + next.action + " at " + iso_utc(next.timestamp));

  if (now < next.timestamp - WAKE_EARLY_SEC) {
    Serial.print("Waiting until ");
    Serial.print((unsigned long)(next.timestamp - WAKE_EARLY_SEC - now));
    Serial.println(" s before event");
    sleep_until_event(next.timestamp);
    // Re-sync time and refresh schedule in case publish finished while we waited
    if (connect_wifi()) {
      sync_ntp();
      if (fetch_schedule_with_retries()) {
        schedule_json = load_cached_schedule();
        ScheduleEvent refreshed = find_next_event(schedule_json);
        if (refreshed.valid) next = refreshed;
      }
      disconnect_wifi();
    }
  }

  now = time(nullptr);
  log_time("Now (pre-run): ", now);

  if (now > next.timestamp + GRACE_PERIOD_SEC) {
    log_line("Event missed — marking done");
    mark_event_done(next.id);
    board_restart_after(5);
  }

  if (now < next.timestamp) {
    Serial.print("Waiting ");
    Serial.print((unsigned long)(next.timestamp - now));
    Serial.println(" s for event time");
    wait_until(next.timestamp);
  } else {
    Serial.print("Running ");
    Serial.print((unsigned long)(now - next.timestamp));
    Serial.println(" s late (within grace)");
  }

  Serial.print("Executing ");
  Serial.println(next.action);
  set_runtime_status("executing", String(next.action) + " in progress");
  led_blink(5, 80, 80);

  uint32_t timeout = next.expected_duration_s * 1000UL;
  if (timeout < 30000) timeout = 30000;
  if (timeout > MOVE_TIMEOUT_MS) timeout = MOVE_TIMEOUT_MS;

  bool ok = move_to_virtual_angle(next.virtual_angle, timeout);
  remember_event_result(next, ok, last_move_detail);
  last_event_time = iso_utc(next.timestamp);
  if (ok) {
    mark_event_done(next.id);
    set_runtime_status("idle", String(next.action) + " complete" + (last_move_detail.length() ? (String(" (") + last_move_detail + ")") : ""));
    log_line("Event complete");
    led_blink(10, 50, 50);
  } else {
    set_runtime_status("error", String(next.action) + " failed" + (last_move_detail.length() ? (String(" (") + last_move_detail + ")") : ""));
    log_line("Event failed — will retry next wake");
    led_blink(3, 500, 200);
  }

  if (connect_wifi()) {
    report_device_status();
    disconnect_wifi();
  }

  board_restart_after(5);
}

void loop() {
  // Device restarts from setup() after each deep sleep
}

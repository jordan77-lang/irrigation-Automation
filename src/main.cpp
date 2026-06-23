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
#include <time.h>

#if __has_include("config.h")
#include "config.h"
#else
#include "config.example.h"
#warning "Using config.example.h — copy include/config.example.h to include/config.h"
#endif

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
  log_line("Schedule fetched and cached");
  return true;
}

static bool load_schedule_json(String& json) {
  if (schedule_cache_stale() || !LittleFS.exists(SCHEDULE_CACHE_PATH)) {
    if (connect_wifi()) {
      fetch_and_cache_schedule();
      disconnect_wifi();
    }
  }
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
  StaticJsonDocument<8192> doc;
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
  time_t horizon = now + (7 * 24 * 3600);

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

    if (ts > horizon) break;
  }

  return best;
}

// ---------------------------------------------------------------------------
// Hardware
// ---------------------------------------------------------------------------

static float read_as5600_degrees() {
  Wire.beginTransmission(0x36);
  Wire.write(0x0E);
  if (Wire.endTransmission() != 0) return -1.0f;
  if (Wire.requestFrom(0x36, 2) != 2) return -1.0f;
  uint16_t raw = (Wire.read() << 8) | Wire.read();
  return (raw & 0x0FFF) * 360.0f / 4096.0f;
}

static bool wait_power_good() {
  for (int i = 0; i < 50; i++) {
    if (digitalRead(PIN_PG) == HIGH) return true;
    delay(100);
  }
  return false;
}

static void init_hardware() {
  Wire.begin(PIN_SDA, PIN_SCL);
  pinMode(PIN_STEP, OUTPUT);
  pinMode(PIN_DIR, OUTPUT);
  pinMode(PIN_EN, OUTPUT);
  pinMode(PIN_PG, INPUT);
  digitalWrite(PIN_STEP, LOW);
  digitalWrite(PIN_EN, HIGH);
  degrees_per_microstep = 360.0f / (STEPS_PER_REV * MICROSTEPS);
}

static bool move_to_virtual_angle(float target, uint32_t timeout_ms) {
  if (!wait_power_good()) {
    log_line("Power not good — skipping move");
    return false;
  }

  float delta = target - current_virtual_position;
  if (fabs(delta) < degrees_per_microstep) {
    current_virtual_position = target;
    prefs.putFloat("virtual_pos", current_virtual_position);
    return true;
  }

  int direction = delta > 0 ? 1 : -1;
  long steps_remaining = labs((long)(delta / degrees_per_microstep));
  digitalWrite(PIN_DIR, direction > 0 ? HIGH : LOW);
  digitalWrite(PIN_EN, LOW);

  uint32_t start = millis();
  const uint32_t step_delay_us = 800;

  while (steps_remaining > 0) {
    if (millis() - start > timeout_ms) {
      log_line("Move timeout");
      digitalWrite(PIN_EN, HIGH);
      return false;
    }
    digitalWrite(PIN_STEP, HIGH);
    delayMicroseconds(step_delay_us);
    digitalWrite(PIN_STEP, LOW);
    delayMicroseconds(step_delay_us);
    steps_remaining--;
  }

  digitalWrite(PIN_EN, HIGH);
  current_virtual_position = target;
  prefs.putFloat("virtual_pos", current_virtual_position);

  float enc = read_as5600_degrees();
  log_f("Encoder deg: ", enc);
  return true;
}

// ---------------------------------------------------------------------------
// Sleep
// ---------------------------------------------------------------------------

static void deep_sleep_seconds(uint64_t seconds) {
  if (seconds < 1) seconds = 1;
  Serial.print("Sleeping ");
  Serial.print((unsigned long)seconds);
  Serial.println(" s");
  Serial.flush();
  disconnect_wifi();
  esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);
  esp_deep_sleep_start();
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
    deep_sleep_seconds((uint64_t)delta);
  }
}

// ---------------------------------------------------------------------------
// Main flow — setup() runs each wake cycle; loop() is unused
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  // USB CDC on ESP32-S3 needs time to enumerate before first prints
  delay(2000);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  Serial.print("Wake cause: ");
  Serial.println((int)cause);

  init_hardware();
  prefs.begin(PREFS_NS, false);
  current_virtual_position = prefs.getFloat("virtual_pos", CLOSED_VIRTUAL_ANGLE);

  if (!LittleFS.begin(true)) {
    log_line("LittleFS mount failed");
  }

  bool need_wifi = ntp_stale() || schedule_cache_stale() || !LittleFS.exists(SCHEDULE_CACHE_PATH);
  if (need_wifi) {
    if (connect_wifi()) {
      if (ntp_stale()) sync_ntp();
      if (schedule_cache_stale() || !LittleFS.exists(SCHEDULE_CACHE_PATH)) {
        fetch_and_cache_schedule();
      }
      disconnect_wifi();
    }
  }

  if (time(nullptr) < 1700000000) {
  // No valid time — retry soon
    if (connect_wifi()) {
      sync_ntp();
      disconnect_wifi();
    }
    if (time(nullptr) < 1700000000) {
      deep_sleep_seconds(300);
    }
  }

  String schedule_json;
  if (!load_schedule_json(schedule_json)) {
    log_line("No schedule available");
    deep_sleep_seconds(3600);
  }

  ScheduleEvent next = find_next_event(schedule_json);
  if (!next.valid) {
    log_line("No upcoming events — sleep 24h");
    deep_sleep_seconds(24 * 3600);
  }

  time_t now = time(nullptr);
  Serial.print("Next event: ");
  Serial.println(next.id);

  if (now < next.timestamp - WAKE_EARLY_SEC) {
    sleep_until_event(next.timestamp);
    if (connect_wifi()) sync_ntp();
    disconnect_wifi();
  }

  now = time(nullptr);
  if (now > next.timestamp + GRACE_PERIOD_SEC) {
    log_line("Event missed — marking done");
    mark_event_done(next.id);
    deep_sleep_seconds(5);
  }

  if (now < next.timestamp) {
    wait_until(next.timestamp);
  }

  Serial.print("Executing ");
  Serial.println(next.action);
  uint32_t timeout = next.expected_duration_s * 1000UL;
  if (timeout < 30000) timeout = 30000;
  if (timeout > MOVE_TIMEOUT_MS) timeout = MOVE_TIMEOUT_MS;

  bool ok = move_to_virtual_angle(next.virtual_angle, timeout);
  if (ok) {
    mark_event_done(next.id);
    log_line("Event complete");
  } else {
    log_line("Event failed — will retry next wake");
  }

  deep_sleep_seconds(5);
}

void loop() {
  // Device restarts from setup() after each deep sleep
}

// ESP32-S3 PD-Stepper Firmware Scaffold
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <TMCStepper.h>
#include <Preferences.h>

// --- CONFIG ---
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* SCHEDULE_URL = "https://raw.githubusercontent.com/jordan77-lang/irrigation-Automation/main/schedules/schedules.json";
const char* SIGNATURE_URL = "https://raw.githubusercontent.com/jordan77-lang/irrigation-Automation/main/schedules/schedules.json.sig";
const char* DEVICE_ID = "pd01";
const char* SIGN_KEY = "YOUR_SIGN_KEY"; // Hex string, provisioned from repo secret

// --- Pinout (edit for your board) ---
#define TMC2209_STEP_PIN  2
#define TMC2209_DIR_PIN   3
#define TMC2209_EN_PIN    4
#define TMC2209_UART_PIN  5
#define AS5600_SDA_PIN    8
#define AS5600_SCL_PIN    9

// --- Globals ---
Preferences prefs;
float current_virtual_position = 0.0; // degrees
float closed_virtual_angle = 0.0;
float open_virtual_angle = 1440.0; // example: 4 turns
float step_to_deg = 0.9; // degrees per step (example)

// --- Helper: HMAC-SHA256 verification ---
// (Use a library or implement HMAC-SHA256 here; placeholder for brevity)
bool verify_hmac(const String& json, const String& sig, const String& key) {
	// TODO: Implement HMAC-SHA256 verification
	return true; // Always true for scaffold
}

// --- Helper: AS5600 angle read ---
float read_as5600_angle() {
	Wire.beginTransmission(0x36); // AS5600 I2C address
	Wire.write(0x0E); // Angle register
	Wire.endTransmission();
	Wire.requestFrom(0x36, 2);
	if (Wire.available() == 2) {
		uint16_t raw = Wire.read() << 8 | Wire.read();
		return (raw & 0x0FFF) * 0.08789; // 0-4095 -> 0-360 deg
	}
	return -1.0;
}

// --- Helper: Stepper move ---
void move_to_virtual_angle(float target_angle) {
	float delta = target_angle - current_virtual_position;
	int direction = (delta >= 0) ? 1 : -1;
	long steps = abs(delta) / step_to_deg;
	digitalWrite(TMC2209_DIR_PIN, direction > 0 ? HIGH : LOW);
	digitalWrite(TMC2209_EN_PIN, LOW); // Enable driver
	for (long i = 0; i < steps; i++) {
		digitalWrite(TMC2209_STEP_PIN, HIGH);
		delayMicroseconds(500);
		digitalWrite(TMC2209_STEP_PIN, LOW);
		delayMicroseconds(500);
		// Optionally read AS5600 and update current_virtual_position
	}
	digitalWrite(TMC2209_EN_PIN, HIGH); // Disable driver
	current_virtual_position = target_angle;
	prefs.putFloat("virtual_position", current_virtual_position);
}

// --- Wi-Fi & NTP ---
void setup_wifi() {
	WiFi.begin(WIFI_SSID, WIFI_PASS);
	while (WiFi.status() != WL_CONNECTED) {
		delay(500);
		Serial.print(".");
	}
	Serial.println("\nWiFi connected");
}

void setup_ntp() {
	configTime(0, 0, "pool.ntp.org", "time.nist.gov");
	Serial.println("Waiting for NTP sync...");
	time_t now = time(nullptr);
	while (now < 100000) {
		delay(500);
		now = time(nullptr);
	}
	Serial.println("NTP time synced");
}

// --- Schedule fetch & parse ---
bool fetch_and_verify_schedule(String& schedule_json) {
	HTTPClient http;
	http.begin(SCHEDULE_URL);
	int code = http.GET();
	if (code == 200) {
		schedule_json = http.getString();
		http.end();
		http.begin(SIGNATURE_URL);
		int sig_code = http.GET();
		if (sig_code == 200) {
			String sig = http.getString();
			http.end();
			return verify_hmac(schedule_json, sig, SIGN_KEY);
		}
		http.end();
	}
	return false;
}

void setup() {
	Serial.begin(115200);
	Wire.begin(AS5600_SDA_PIN, AS5600_SCL_PIN);
	pinMode(TMC2209_STEP_PIN, OUTPUT);
	pinMode(TMC2209_DIR_PIN, OUTPUT);
	pinMode(TMC2209_EN_PIN, OUTPUT);
	digitalWrite(TMC2209_EN_PIN, HIGH); // Disable driver
	prefs.begin("pdstepper", false);
	current_virtual_position = prefs.getFloat("virtual_position", 0.0);
	setup_wifi();
	setup_ntp();
}

void loop() {
	String schedule_json;
	if (fetch_and_verify_schedule(schedule_json)) {
		// Parse JSON and find next event for DEVICE_ID
		StaticJsonDocument<4096> doc;
		DeserializationError err = deserializeJson(doc, schedule_json);
		if (!err) {
			JsonArray events = doc["devices"][DEVICE_ID].as<JsonArray>();
			for (JsonObject evt : events) {
				String action = evt["action"].as<String>();
				float target_angle = evt["virtual_angle"].as<float>();
				String time_str = evt["time"].as<String>();
				// Convert time_str to time_t, compare to now
				// If event is due, execute move
				// For brevity, just move immediately here
				move_to_virtual_angle(target_angle);
				delay(10000); // Wait 10s before next event
			}
		}
	} else {
		Serial.println("Failed to fetch or verify schedule");
		delay(60000); // Retry in 1 min
	}
}
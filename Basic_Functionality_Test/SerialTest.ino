// Simple ESP32-S3 Serial Test Sketch
void setup() {
  Serial.begin(115200);
  Serial.println("ESP32-S3 Serial Test: Board is running!");
}

void loop() {
  Serial.println("Hello from ESP32-S3!");
  delay(1000);
}

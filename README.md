# ESP32-S3 PD-Stepper Firmware

This PlatformIO project controls a PD-Stepper for irrigation automation:
- Connects to Wi-Fi
- Syncs time via NTP
- Fetches and verifies signed schedules from GitHub
- Drives stepper for multi-turn open/close using AS5600 feedback
- Stores current position in Preferences

Edit `src/main.cpp` for your pinout and device config.

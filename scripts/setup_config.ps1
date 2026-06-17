# Create include/config.h from config.example.h (local only, gitignored)
$root = Split-Path -Parent $PSScriptRoot
$example = Join-Path $root "include\config.example.h"
$target = Join-Path $root "include\config.h"

if (-not (Test-Path $example)) {
  Write-Error "Missing $example"
  exit 1
}

if (Test-Path $target) {
  Write-Host "include/config.h already exists - not overwriting." -ForegroundColor Yellow
  Write-Host "Edit it manually or delete it and run this script again."
  exit 0
}

Copy-Item $example $target
Write-Host "Created include/config.h" -ForegroundColor Green
Write-Host ""
Write-Host "Edit include/config.h and set:"
Write-Host "  - WIFI_SSID / WIFI_PASS"
Write-Host "  - SIGN_KEY (run .\scripts\generate_sign_key.ps1 first)"
Write-Host ""
Write-Host "Then build: pio run"
Write-Host "See SETUP.md and FLASHING.md for full steps."

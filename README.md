# 🕐 Marble Ring Clock

**ESP32 Lolin32 Lite** firmware for a WS2812B LED ring wall clock using a marble ring (Ø410mm, 25mm thick).

Two LEDs light up: one for the **hour hand**, one for the **minute hand**. Configurable colors, brightness, and LED count. Time synced via **NTP over WiFi**. Includes a **hosted web UI** for all settings.

---

## Hardware

| Component | Details |
|---|---|
| MCU | ESP32 Lolin32 Lite |
| LEDs | WS2812B strip, 60 LED/m (configurable) |
| Ring | Marble, Ø410mm outer, 25mm wide |
| Data pin | GPIO 22 (configurable in firmware) |
| Power | External 5V PSU for LEDs + ESP32 |

### LED count estimation

With a ring of ~370mm working diameter and a 60 LED/m strip:
```
Circumference ≈ π × 370 = ~1162 mm
LEDs ≈ 1162 / 1000 × 60 ≈ 70 LEDs
```

The firmware handles any number of LEDs with 3 mapping modes:
- **Distribute evenly** (recommended): maps hour/minute positions across all physical LEDs
- **First 60 only**: uses the first 60 LEDs directly (some LEDs dark)
- **All with gaps**: spreads all LEDs with even spacing

---

## Features

- 🌐 **NTP time sync** (pool.ntp.org)
- 🎨 **Independent color** for hour and minute hands (RGB color picker)
- 💡 **Independent brightness** for each hand + global brightness
- ⚙️ **Web UI** hosted on ESP32 (LittleFS)
- 📐 **Configurable LED count** and mapping mode
- 🕐 **Manual time override** (when no WiFi)
- 🔄 **OTA firmware updates** via ArduinoOTA
- 📡 **AP mode fallback**: if WiFi fails, creates `MarbleClock` hotspot (pass: `clock1234`)

---

## Building & Flashing

### Requirements

- [PlatformIO](https://platformio.org/) (VS Code extension recommended)
- ESP32 Arduino core 2.x

### Steps

```bash
cd firmware/marble_clock

# Build & flash firmware
pio run --target upload

# Build & flash filesystem (web UI)
pio run --target uploadfs
```

> **First time**: flash filesystem first, then firmware.

### Arduino IDE (alternative)

1. Install libraries: FastLED, ESPAsyncWebServer, AsyncTCP, NTPClient, ArduinoJson
2. In Tools → Board: `ESP32 Dev Module` (closest to Lolin32 Lite)
3. Upload filesystem via ESP32 Sketch Data Upload plugin
4. Upload sketch

---

## Web UI

After flashing, connect to the same WiFi as the ESP32 (or to the `MarbleClock` AP).

Open a browser and go to the IP shown in Serial Monitor (or `http://marble-clock.local`).

The web app allows:
- Setting WiFi credentials
- Configuring number of LEDs and mapping mode
- Adjusting global brightness
- Setting hour/minute hand colors independently
- Setting hour/minute hand brightness independently
- Configuring timezone (UTC offset)
- Forcing NTP resync
- Manual time override
- **LED calculator**: input ring diameter → shows estimated LED count

---

## Wiring

```
 ESP32 Lolin32 Lite          WS2812B Strip
  GPIO 22 ────────────────── DIN
  GND     ────────────────── GND ──── PSU GND
                             5V  ──── PSU 5V (+)
  VIN/3V3 ─── (ESP32 only, from USB or regulator)
```

> ⚠ **Important**: connect a 300-500Ω resistor in series on the data line. Add a 1000µF capacitor between 5V and GND near the LED strip. Never power the LED strip directly from the ESP32's 5V pin if using many LEDs.

### Power consumption estimate

| Config | Current |
|---|---|
| 70 LEDs, 2 lit at full white | ~120 mA |
| 70 LEDs, all lit (max) | ~4.2 A |
| Typical (2 LEDs at 50% bri) | ~80 mA |

A **2A 5V PSU** is more than enough for normal use.

---

## API Reference

| Endpoint | Method | Description |
|---|---|---|
| `/api/status` | GET | Returns current time, config, and WiFi/NTP state as JSON |
| `/api/config` | POST | Update any config fields (JSON body) |
| `/api/syncntp` | POST | Force NTP resync |
| `/api/reconnect` | POST | Restart ESP32 to apply new WiFi credentials |

---

## 3D Print Notes

The ring holder should:
- Support the marble ring at the back
- Leave a ~5-10mm channel inside the ring edge for the LED strip
- Include cable routing for data + power wires
- Use **PETG or ASA** (not PLA) for longevity near heat

---

## License

MIT — free to use, modify and distribute.

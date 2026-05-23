# Smart Central Heating Controller

A cheap, easy-to-assemble upgrade for old radio-controlled central heating units. Replace the RF remote with two small Wi-Fi boards — one reads room temperature and drives a web dashboard, the other physically switches the boiler via a relay.

---

## How It Works

```
┌─────────────────────┐        HTTP over LAN        ┌──────────────────────┐
│  Thermostat Unit    │  ──────────────────────────► │  Relay Control Unit  │
│  (ESP32)            │   GET /turn-heater-on/off    │  (ESP8266)           │
│                     │                              │                      │
│  • DHT11 sensor     │                              │  • Controls relay    │
│  • Web dashboard    │                              │  • thermostat.local  │
│  • thermostat.local │                              │  • relay.local       │
└─────────────────────┘                              └──────────────────────┘
                                                              │
                                                         relay contact
                                                              │
                                                         Boiler / Heater
```

Every 5 seconds the thermostat reads the sensor and compares it to the desired temperature. If the room is too cold it calls `http://relay.local/turn-heater-on`; once it reaches the setpoint it calls `http://relay.local/turn-heater-off`. No cloud, no app, no subscription — just two devices talking on your local network.

---

## Hardware

### Thermostat Unit — `thermostat-esp32/`

| Component | Detail |
|-----------|--------|
| MCU | ESP32 dev board |
| Temperature/humidity sensor | DHT11 on GPIO 4 |
| Status LED | GPIO 2 (built-in) |
| Factory-reset button | GPIO 0 (BOOT button) |

### Relay Control Unit — `IoT-ESP8266/`

| Component | Detail |
|-----------|--------|
| MCU | ESP8266 ESP-12E |
| Relay | D1 (GPIO 5), wired as `OUTPUT_OPEN_DRAIN` for active-low relay modules |
| Status LED | `LED_BUILTIN` (active low) |
| Factory-reset button | D3 (GPIO 0) |

> **Safety note:** The relay pin is immediately driven HIGH on boot so the heater starts in the **off** state, even during a power-cycle or crash.

---

## Features

- **Zero-config first-time setup** — both units open a captive-portal Wi-Fi hotspot (`Thermostat_Setup` / `Relay_Setup`) on first boot. Connect with any phone or laptop, enter your Wi-Fi credentials, and they're online.
- **mDNS hostnames** — access everything at `http://thermostat.local` and `http://relay.local` (no IP address needed).
- **Web dashboard** — live temperature & humidity readings with a 5-minute rolling chart, desired-temperature input, Wi-Fi signal strength, and uptime. Light/dark mode with local-storage persistence.
- **Thermostat control loop** — automatic heater on/off based on the current vs. desired temperature, polled every 5 seconds.
- **Factory reset** — hold the BOOT button for 1 second to wipe saved Wi-Fi credentials and reboot into setup mode.
- **Debug mode** — set `#define DEBUG 1` in the thermostat firmware to feed mock temperatures over Serial instead of using the sensor.

---

## Web Dashboard

Open `http://thermostat.local` in a browser on the same network.

- **Desired Temp** — type a value and press Enter/Tab to push it to the device immediately.
- **Current temperature / Humidity** — live readings from the DHT11.
- **Charts** — canvas-rendered 5-minute rolling history for temperature and humidity.
- **Status bar** — Wi-Fi RSSI, uptime, and a link to `thermostat.local`.

---

## API Reference

### Thermostat (`thermostat.local`)

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/` | Web dashboard (HTML) |
| `GET` | `/api/sensor` | Current temperature and humidity as JSON |
| `GET` | `/api/history` | Rolling 5-minute history (temperature + humidity arrays) |
| `GET` | `/api/status` | Wi-Fi RSSI, uptime, hostname, IP |
| `GET` | `/api/set-desired-tmp?desired-tmp=22` | Set target temperature (°C) |
| `GET` | `/api/get-desired-tmp` | Get current target temperature |

**Example — `/api/sensor` response:**
```json
{ "ok": true, "temperature_c": 21.5, "humidity_pct": 48.0 }
```

**Example — `/api/history` response:**
```json
{ "ok": true, "n": 60, "s": [295, 290, ...], "tc": [21.5, 21.4, ...], "rh": [48.0, 47.8, ...] }
```

### Relay (`relay.local`)

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/turn-heater-on` | Close relay contact (heater on) |
| `GET` | `/turn-heater-off` | Open relay contact (heater off) |

---

## Build & Flash

Both projects use [PlatformIO](https://platformio.org/).

### Prerequisites

- PlatformIO Core (CLI) or the PlatformIO IDE extension for VS Code
- USB cable for each board

### Thermostat (ESP32)

```bash
cd thermostat-esp32

# Build and flash firmware
pio run --target upload

# Upload the web UI to SPIFFS
pio run --target uploadfs

# Open serial monitor
pio device monitor
```

### Relay (ESP8266)

```bash
cd IoT-ESP8266

# Build and flash firmware
pio run --target upload

# Open serial monitor
pio device monitor
```

### Dependencies

| Unit | Library |
|------|---------|
| Thermostat (ESP32) | `adafruit/DHT sensor library`, `adafruit/Adafruit Unified Sensor` |
| Relay (ESP8266) | Arduino core only (no extra libraries) |

---

## First-Time Setup

1. Flash both boards.
2. **Thermostat:** A Wi-Fi network called **`Thermostat_Setup`** will appear. Connect to it, open a browser, and navigate to `192.168.4.1`. Enter your home Wi-Fi credentials and click **Save & Connect**. The board reboots and joins your network.
3. **Relay:** Same process — connect to **`Relay_Setup`** and enter credentials.
4. Both boards will register as `thermostat.local` and `relay.local` via mDNS.
5. Open `http://thermostat.local` — the dashboard should be live within seconds.

> If you need to re-run setup (e.g., changed router password), hold the **BOOT button** for 1 second to factory reset either board.

---

## Project Structure

```
smart-central-heating-controller/
├── thermostat-esp32/       # ESP32 thermostat + web UI
│   ├── src/main.cpp        # Firmware
│   ├── data/index.html     # Web dashboard (flashed to SPIFFS)
│   └── platformio.ini
├── IoT-ESP8266/            # ESP8266 relay controller
│   ├── src/main.cpp        # Firmware
│   └── platformio.ini
└── README.md
```

---

## License

See [LICENSE](LICENSE).
